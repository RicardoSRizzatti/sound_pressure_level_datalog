/***************************************************************************//**
 * @file
 * @brief NVM3-backed measurement datalog implementation.
 *
 * Records are stored in BATCHES of SPL_STORE_BATCH records per NVM3 object
 * (ring of SPL_STORE_MAX_RECORDS / SPL_STORE_BATCH objects). Compared to one
 * object per record this cuts the boot scan and repack pressure by 10x —
 * a full ring of tiny objects once made NVM3 maintenance exceed the
 * watchdog period (boot-loop bug, 2026-07-05).
 *
 * The current (partial) batch is rewritten on every append, so every record
 * is persisted immediately. ACK deletion is batch-granular: records of a
 * partially-acknowledged batch can reappear after a reboot — the app
 * deduplicates by (boot_id, seq), as required by docs/protocolo_ble.md.
 ******************************************************************************/
#include "spl_store.h"

#include <string.h>

#include "app_watchdog.h"
#include "nvm3_default.h"

// NVM3 key map. Batch objects occupy [KEY_RECORD_BASE, KEY_RECORD_BASE + MAX_BATCHES);
// spectrum batches occupy [KEY_SPEC_BASE, KEY_SPEC_BASE + MAX_SPEC_BATCHES).
#define KEY_BOOT_ID        0x0001u
#define KEY_WDOG_COUNT     0x0002u
// 0x0003 is the device configuration (spl_config.c).
#define KEY_BOOT_EPOCHS    0x0004u
#define KEY_RECORD_BASE    0x1000u
#define KEY_SPEC_BASE      0x2000u
#define KEY_EXT_BASE       0x3000u

#define BATCH              SPL_STORE_BATCH
#define MAX_BATCHES        (SPL_STORE_MAX_RECORDS / SPL_STORE_BATCH)
#define SPEC_BATCH         SPL_STORE_SPEC_BATCH
#define MAX_SPEC_BATCHES   (SPL_STORE_MAX_SPECTRA / SPL_STORE_SPEC_BATCH)
#define EXT_BATCH          SPL_STORE_EXT_BATCH
#define MAX_EXT_BATCHES    (SPL_STORE_MAX_EXTENDED / SPL_STORE_EXT_BATCH)

// Largest companion-ring object in bytes (spectrum: 68 B x 3).
#define COMPANION_MAX_OBJ  (sizeof(spl_spectrum_t) * SPL_STORE_SPEC_BATCH)

static uint32_t boot_id;
static uint32_t wdog_reset_count;
static uint32_t next_seq;    // seq the next appended record will get
static uint32_t oldest_seq;  // seq of the oldest record still served
static bool empty = true;

// Batch currently being filled (rewritten on every append).
static spl_record_t cur_batch[BATCH];

// One-slot read cache: sequential readers (sync/dump) hit it 9 times in 10.
static spl_record_t read_cache[BATCH];
static uint32_t read_cache_batch = UINT32_MAX;
static uint32_t read_cache_count;

// Generic companion ring: parallel to the record ring, keyed by seq, with
// fixed-size entries whose first 8 bytes are {boot_id u32, seq u32}. Used
// for both the 1/3-octave spectra and the extended B&K metrics. Each ring
// keeps the currently-filled batch in RAM (rewritten on every append).
typedef struct {
  nvm3_ObjectKey_t key_base;
  uint32_t max_batches;
  uint32_t batch;          // records per NVM3 object
  size_t entry_size;
  uint8_t cur[COMPANION_MAX_OBJ];
  uint32_t cur_count;
  uint32_t cur_batch;      // batch index of cur[], UINT32_MAX when empty
} companion_ring_t;

static companion_ring_t spec_ring = {
  .key_base = KEY_SPEC_BASE, .max_batches = MAX_SPEC_BATCHES,
  .batch = SPEC_BATCH, .entry_size = sizeof(spl_spectrum_t),
  .cur_batch = UINT32_MAX,
};
static companion_ring_t ext_ring = {
  .key_base = KEY_EXT_BASE, .max_batches = MAX_EXT_BATCHES,
  .batch = EXT_BATCH, .entry_size = sizeof(spl_extended_t),
  .cur_batch = UINT32_MAX,
};

static void companion_delete_through(companion_ring_t *r, uint32_t seq);
static nvm3_ObjectKey_t companion_key(const companion_ring_t *r, uint32_t batch);

static nvm3_ObjectKey_t batch_key(uint32_t batch)
{
  return KEY_RECORD_BASE + (batch % MAX_BATCHES);
}

static bool read_counter(nvm3_ObjectKey_t key, uint32_t *value)
{
  uint32_t type;
  size_t len;
  if (nvm3_getObjectInfo(nvm3_defaultHandle, key, &type, &len) != SL_STATUS_OK
      || type != NVM3_OBJECTTYPE_DATA || len != sizeof(*value)) {
    return false;
  }
  return nvm3_readData(nvm3_defaultHandle, key, value, sizeof(*value)) == SL_STATUS_OK;
}

/// Read the batch object at ring slot @p slot; returns record count (0 if none).
static uint32_t read_batch_slot(uint32_t slot, spl_record_t *out)
{
  uint32_t type;
  size_t len;
  nvm3_ObjectKey_t key = KEY_RECORD_BASE + slot;
  if (nvm3_getObjectInfo(nvm3_defaultHandle, key, &type, &len) != SL_STATUS_OK
      || type != NVM3_OBJECTTYPE_DATA
      || len == 0 || len > sizeof(spl_record_t) * BATCH
      || (len % sizeof(spl_record_t)) != 0) {
    return 0;
  }
  if (nvm3_readData(nvm3_defaultHandle, key, out, len) != SL_STATUS_OK) {
    return 0;
  }
  return (uint32_t)(len / sizeof(spl_record_t));
}

bool spl_store_init(void)
{
  // Boot id: increment a persistent counter every boot.
  uint32_t prev = 0;
  (void)read_counter(KEY_BOOT_ID, &prev);
  boot_id = prev + 1;
  if (nvm3_writeData(nvm3_defaultHandle, KEY_BOOT_ID, &boot_id, sizeof(boot_id))
      != SL_STATUS_OK) {
    return false;
  }

  // Watchdog reset counter.
  (void)read_counter(KEY_WDOG_COUNT, &wdog_reset_count);
  if (app_watchdog_reset_was_watchdog()) {
    wdog_reset_count++;
    (void)nvm3_writeData(nvm3_defaultHandle, KEY_WDOG_COUNT,
                         &wdog_reset_count, sizeof(wdog_reset_count));
  }

  // Recover ring state: scan the batch objects for min/max sequence numbers.
  uint32_t min_seq = UINT32_MAX;
  uint32_t max_seq = 0;
  bool found = false;
  spl_record_t buf[BATCH];
  for (uint32_t slot = 0; slot < MAX_BATCHES; slot++) {
    if ((slot & 0x1F) == 0) {
      app_watchdog_feed_now();   // NVM3 lookups can be slow with a full ring
    }
    uint32_t n = read_batch_slot(slot, buf);
    if (n > 0) {
      found = true;
      if (buf[0].seq < min_seq) {
        min_seq = buf[0].seq;
      }
      if (buf[n - 1].seq > max_seq) {
        max_seq = buf[n - 1].seq;
      }
    }
  }

  empty = !found;
  next_seq = found ? max_seq + 1 : 0;
  oldest_seq = found ? min_seq : 0;

  // Reload the partial batch being filled, if any.
  if (found && (next_seq % BATCH) != 0) {
    (void)read_batch_slot(batch_key(next_seq / BATCH) - KEY_RECORD_BASE, cur_batch);
  }
  return true;
}

bool spl_store_append(int16_t laeq_cdb, int16_t lafmax_cdb, uint32_t uptime_s)
{
  uint32_t batch = next_seq / BATCH;
  uint32_t idx = next_seq % BATCH;

  cur_batch[idx] = (spl_record_t){
    .boot_id = boot_id,
    .seq = next_seq,
    .uptime_s = uptime_s,
    .laeq_cdb = laeq_cdb,
    .lafmax_cdb = lafmax_cdb,
  };

  if (nvm3_writeData(nvm3_defaultHandle, batch_key(batch),
                     cur_batch, (idx + 1) * sizeof(spl_record_t)) != SL_STATUS_OK) {
    app_watchdog_set_storage_healthy(false);
    return false;
  }
  if (read_cache_batch == batch) {
    read_cache_batch = UINT32_MAX;   // stale
  }

  if (empty) {
    oldest_seq = next_seq;
    empty = false;
  } else if (idx == 0 && batch >= MAX_BATCHES) {
    // Starting a new batch overwrote the oldest slot: 10 records gone at once.
    uint32_t min_alive = (batch - MAX_BATCHES + 1) * BATCH;
    if (oldest_seq < min_alive) {
      oldest_seq = min_alive;
    }
  }
  next_seq++;
  app_watchdog_set_storage_healthy(true);
  return true;
}

uint32_t spl_store_count(void)
{
  return empty ? 0 : (next_seq - oldest_seq);
}

uint32_t spl_store_oldest_seq(void)
{
  return oldest_seq;
}

uint32_t spl_store_next_seq(void)
{
  return next_seq;
}

bool spl_store_read(uint32_t seq, spl_record_t *out)
{
  if (empty || seq < oldest_seq || seq >= next_seq) {
    return false;
  }

  uint32_t batch = seq / BATCH;
  uint32_t idx = seq % BATCH;

  if (read_cache_batch != batch) {
    read_cache_count = read_batch_slot(batch_key(batch) - KEY_RECORD_BASE, read_cache);
    read_cache_batch = (read_cache_count > 0) ? batch : UINT32_MAX;
  }
  if (read_cache_batch != batch || idx >= read_cache_count) {
    return false;
  }
  *out = read_cache[idx];
  return out->seq == seq;
}

void spl_store_delete_through(uint32_t seq)
{
  if (empty || seq < oldest_seq) {
    return;
  }
  if (seq >= next_seq) {
    seq = next_seq - 1;
  }

  // Delete only batches fully covered by the ACK; a partial batch stays on
  // flash (its already-acknowledged records may reappear after a reboot —
  // the app deduplicates by boot_id+seq).
  uint32_t first_batch = oldest_seq / BATCH;
  uint32_t done = 0;
  for (uint32_t b = first_batch; (b * BATCH + BATCH - 1) <= seq; b++) {
    if (((done++) & 0x1F) == 0) {
      app_watchdog_feed_now();
    }
    (void)nvm3_deleteObject(nvm3_defaultHandle, batch_key(b));
    if (read_cache_batch == b) {
      read_cache_batch = UINT32_MAX;
    }
  }

  companion_delete_through(&spec_ring, seq);
  companion_delete_through(&ext_ring, seq);

  oldest_seq = seq + 1;
  empty = (oldest_seq == next_seq);
}

void spl_store_clear(void)
{
  if (!empty) {
    spl_store_delete_through(next_seq - 1);
    // Also drop the partial batch objects, if any.
    (void)nvm3_deleteObject(nvm3_defaultHandle, batch_key(next_seq / BATCH));
    (void)nvm3_deleteObject(nvm3_defaultHandle, companion_key(&spec_ring, next_seq / SPEC_BATCH));
    (void)nvm3_deleteObject(nvm3_defaultHandle, companion_key(&ext_ring, next_seq / EXT_BATCH));
    read_cache_batch = UINT32_MAX;
    spec_ring.cur_batch = UINT32_MAX;
    ext_ring.cur_batch = UINT32_MAX;
    oldest_seq = next_seq;
    empty = true;
  }
}

void spl_store_maintain(void)
{
  if (nvm3_repackNeeded(nvm3_defaultHandle)) {
    (void)nvm3_repack(nvm3_defaultHandle);   // one staged increment
  }
}

uint32_t spl_store_boot_id(void)
{
  return boot_id;
}

uint32_t spl_store_watchdog_reset_count(void)
{
  return wdog_reset_count;
}

// ---- Generic companion ring --------------------------------------------------

// Byte offset of the seq field inside every companion entry ({boot_id, seq}).
#define COMPANION_SEQ_OFFSET 4u

static nvm3_ObjectKey_t companion_key(const companion_ring_t *r, uint32_t batch)
{
  return r->key_base + (batch % r->max_batches);
}

static uint32_t entry_seq(const uint8_t *entry)
{
  uint32_t seq;
  memcpy(&seq, entry + COMPANION_SEQ_OFFSET, sizeof(seq));
  return seq;
}

/// Read a companion object into @p out; returns the entry count (0 if none).
static uint32_t companion_read_object(const companion_ring_t *r,
                                      nvm3_ObjectKey_t key, uint8_t *out)
{
  uint32_t type;
  size_t len;
  if (nvm3_getObjectInfo(nvm3_defaultHandle, key, &type, &len) != SL_STATUS_OK
      || type != NVM3_OBJECTTYPE_DATA
      || len == 0 || len > r->entry_size * r->batch
      || (len % r->entry_size) != 0) {
    return 0;
  }
  if (nvm3_readData(nvm3_defaultHandle, key, out, len) != SL_STATUS_OK) {
    return 0;
  }
  return (uint32_t)(len / r->entry_size);
}

/// Append @p entry (whose seq field must equal @p seq) to ring @p r.
static bool companion_append(companion_ring_t *r, uint32_t seq, const void *entry)
{
  uint32_t batch = seq / r->batch;

  if (r->cur_batch != batch) {
    // Entering a new batch slot: keep entries already stored for THIS batch
    // (reboot recovery); anything older in the slot gets overwritten.
    uint8_t tmp[COMPANION_MAX_OBJ];
    uint32_t n = companion_read_object(r, companion_key(r, batch), tmp);
    r->cur_count = 0;
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t *e = tmp + i * r->entry_size;
      uint32_t s = entry_seq(e);
      if (s / r->batch == batch && s < seq) {
        memcpy(r->cur + r->cur_count * r->entry_size, e, r->entry_size);
        r->cur_count++;
      }
    }
    r->cur_batch = batch;
  }

  if (r->cur_count >= r->batch) {
    return false;   // should not happen (batch seqs per object)
  }
  memcpy(r->cur + r->cur_count * r->entry_size, entry, r->entry_size);
  r->cur_count++;

  return nvm3_writeData(nvm3_defaultHandle, companion_key(r, batch),
                        r->cur, r->cur_count * r->entry_size) == SL_STATUS_OK;
}

/// Copy the entry for @p seq into @p out. False if not stored (or overwritten).
static bool companion_read(companion_ring_t *r, uint32_t seq, void *out)
{
  uint8_t buf[COMPANION_MAX_OBJ];
  uint32_t n = companion_read_object(r, companion_key(r, seq / r->batch), buf);
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *e = buf + i * r->entry_size;
    if (entry_seq(e) == seq) {
      memcpy(out, e, r->entry_size);
      return true;
    }
  }
  return false;
}

/// Delete companion batches fully covered by an ACK up to @p seq.
static void companion_delete_through(companion_ring_t *r, uint32_t seq)
{
  uint32_t last_batch = seq / r->batch;
  uint32_t first_batch = (last_batch >= r->max_batches)
                         ? last_batch - r->max_batches + 1 : 0;
  uint32_t done = 0;
  for (uint32_t b = first_batch; b <= last_batch; b++) {
    if ((b * r->batch + r->batch - 1) > seq) {
      break;   // partially covered batch stays (app dedups on re-sync)
    }
    if (((done++) & 0x1F) == 0) {
      app_watchdog_feed_now();
    }
    (void)nvm3_deleteObject(nvm3_defaultHandle, companion_key(r, b));
    if (r->cur_batch == b) {
      r->cur_batch = UINT32_MAX;
    }
  }
}

// ---- Spectrum & extended companion rings (thin wrappers) ---------------------

bool spl_store_append_spectrum(uint32_t seq, const int16_t bands_cdb[SPL_STORE_SPEC_BANDS])
{
  spl_spectrum_t e = { .boot_id = boot_id, .seq = seq };
  memcpy(e.bands_cdb, bands_cdb, sizeof(e.bands_cdb));
  return companion_append(&spec_ring, seq, &e);
}

bool spl_store_read_spectrum(uint32_t seq, spl_spectrum_t *out)
{
  return companion_read(&spec_ring, seq, out);
}

bool spl_store_append_extended(uint32_t seq, const spl_extended_t *ext)
{
  spl_extended_t e = *ext;
  e.boot_id = boot_id;
  e.seq = seq;
  return companion_append(&ext_ring, seq, &e);
}

bool spl_store_read_extended(uint32_t seq, spl_extended_t *out)
{
  return companion_read(&ext_ring, seq, out);
}

static uint32_t load_boot_epochs(spl_boot_epoch_t table[SPL_STORE_MAX_BOOT_EPOCHS])
{
  uint32_t type;
  size_t len;
  if (nvm3_getObjectInfo(nvm3_defaultHandle, KEY_BOOT_EPOCHS, &type, &len) != SL_STATUS_OK
      || type != NVM3_OBJECTTYPE_DATA
      || len == 0
      || len > sizeof(spl_boot_epoch_t) * SPL_STORE_MAX_BOOT_EPOCHS
      || (len % sizeof(spl_boot_epoch_t)) != 0) {
    return 0;
  }
  if (nvm3_readData(nvm3_defaultHandle, KEY_BOOT_EPOCHS, table, len) != SL_STATUS_OK) {
    return 0;
  }
  return (uint32_t)(len / sizeof(spl_boot_epoch_t));
}

bool spl_store_set_boot_epoch(uint32_t epoch_boot)
{
  spl_boot_epoch_t table[SPL_STORE_MAX_BOOT_EPOCHS];
  uint32_t n = load_boot_epochs(table);

  // Update in place when this boot already has an entry.
  for (uint32_t i = 0; i < n; i++) {
    if (table[i].boot_id == boot_id) {
      table[i].epoch_boot = epoch_boot;
      return nvm3_writeData(nvm3_defaultHandle, KEY_BOOT_EPOCHS,
                            table, n * sizeof(table[0])) == SL_STATUS_OK;
    }
  }

  if (n == SPL_STORE_MAX_BOOT_EPOCHS) {
    // Drop the oldest entry (index 0) to make room.
    for (uint32_t i = 1; i < n; i++) {
      table[i - 1] = table[i];
    }
    n--;
  }
  table[n].boot_id = boot_id;
  table[n].epoch_boot = epoch_boot;
  n++;
  return nvm3_writeData(nvm3_defaultHandle, KEY_BOOT_EPOCHS,
                        table, n * sizeof(table[0])) == SL_STATUS_OK;
}

uint32_t spl_store_get_boot_epochs(spl_boot_epoch_t out[SPL_STORE_MAX_BOOT_EPOCHS])
{
  return load_boot_epochs(out);
}
