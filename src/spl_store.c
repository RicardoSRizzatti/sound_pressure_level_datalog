/***************************************************************************//**
 * @file
 * @brief NVM3-backed measurement datalog implementation.
 ******************************************************************************/
#include "spl_store.h"

#include "app_watchdog.h"
#include "nvm3_default.h"

// NVM3 key map. Records occupy [KEY_RECORD_BASE, KEY_RECORD_BASE + MAX).
#define KEY_BOOT_ID        0x0001u
#define KEY_WDOG_COUNT     0x0002u
// 0x0003 is the device configuration (spl_config.c).
#define KEY_BOOT_EPOCHS    0x0004u
#define KEY_RECORD_BASE    0x1000u

static uint32_t boot_id;
static uint32_t wdog_reset_count;
static uint32_t next_seq;    // seq the next appended record will get
static uint32_t oldest_seq;  // seq of the oldest record still stored
static bool empty = true;

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

static bool read_record_at_slot(uint32_t slot, spl_record_t *rec)
{
  uint32_t type;
  size_t len;
  nvm3_ObjectKey_t key = KEY_RECORD_BASE + slot;
  if (nvm3_getObjectInfo(nvm3_defaultHandle, key, &type, &len) != SL_STATUS_OK
      || type != NVM3_OBJECTTYPE_DATA || len != sizeof(*rec)) {
    return false;
  }
  return nvm3_readData(nvm3_defaultHandle, key, rec, sizeof(*rec)) == SL_STATUS_OK;
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

  // Recover ring state: scan stored records for min/max sequence numbers.
  uint32_t min_seq = UINT32_MAX;
  uint32_t max_seq = 0;
  bool found = false;
  spl_record_t rec;
  for (uint32_t slot = 0; slot < SPL_STORE_MAX_RECORDS; slot++) {
    if (read_record_at_slot(slot, &rec)) {
      found = true;
      if (rec.seq < min_seq) {
        min_seq = rec.seq;
      }
      if (rec.seq > max_seq) {
        max_seq = rec.seq;
      }
    }
  }

  empty = !found;
  next_seq = found ? max_seq + 1 : 0;
  oldest_seq = found ? min_seq : 0;
  return true;
}

bool spl_store_append(int16_t laeq_cdb, int16_t lafmax_cdb, uint32_t uptime_s)
{
  spl_record_t rec = {
    .boot_id = boot_id,
    .seq = next_seq,
    .uptime_s = uptime_s,
    .laeq_cdb = laeq_cdb,
    .lafmax_cdb = lafmax_cdb,
  };

  nvm3_ObjectKey_t key = KEY_RECORD_BASE + (next_seq % SPL_STORE_MAX_RECORDS);
  if (nvm3_writeData(nvm3_defaultHandle, key, &rec, sizeof(rec)) != SL_STATUS_OK) {
    app_watchdog_set_storage_healthy(false);
    return false;
  }

  if (empty) {
    oldest_seq = next_seq;
    empty = false;
  } else if (next_seq - oldest_seq >= SPL_STORE_MAX_RECORDS) {
    oldest_seq = next_seq - SPL_STORE_MAX_RECORDS + 1;  // overwrote the oldest
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
  if (!read_record_at_slot(seq % SPL_STORE_MAX_RECORDS, out)) {
    return false;
  }
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
  for (uint32_t s = oldest_seq; s <= seq; s++) {
    (void)nvm3_deleteObject(nvm3_defaultHandle,
                            KEY_RECORD_BASE + (s % SPL_STORE_MAX_RECORDS));
  }
  oldest_seq = seq + 1;
  empty = (oldest_seq == next_seq);
}

void spl_store_clear(void)
{
  if (!empty) {
    spl_store_delete_through(next_seq - 1);
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
