/***************************************************************************//**
 * @file
 * @brief NVM3-backed measurement datalog (ring of fixed-size records).
 *
 * Records live in NVM3 keys SPL_STORE_KEY_BASE + (seq % SPL_STORE_MAX_RECORDS)
 * so the newest record overwrites the oldest once the ring is full. The next
 * sequence number is recovered at boot by scanning the ring, so a watchdog
 * reset never corrupts or loses confirmed records.
 ******************************************************************************/
#ifndef SPL_STORE_H
#define SPL_STORE_H

#include <stdbool.h>
#include <stdint.h>

/// Ring capacity. 2000 records = 11 h of history at one record / 20 s.
#define SPL_STORE_MAX_RECORDS 2000u

/// Records per NVM3 object (batching keeps boot scan and repack cheap).
#define SPL_STORE_BATCH 10u

/// One measurement record, 16 bytes packed.
typedef struct __attribute__((packed)) {
  uint32_t boot_id;     ///< Increments every boot (records survive resets)
  uint32_t seq;         ///< Global sequence number, monotonic
  uint32_t uptime_s;    ///< Seconds since boot when the interval closed
  int16_t laeq_cdb;     ///< LAeq in centi-dB(A)  (e.g. 6234 = 62.34 dB)
  int16_t lafmax_cdb;   ///< LAFmax in centi-dB(A)
} spl_record_t;

/// Mount counters (boot_id, next seq) from NVM3. Returns false on NVM3 error.
bool spl_store_init(void);

/// Append one measurement. Returns false on write failure.
bool spl_store_append(int16_t laeq_cdb, int16_t lafmax_cdb, uint32_t uptime_s);

/// Number of records currently in the ring.
uint32_t spl_store_count(void);

/// Sequence number of the oldest / newest stored record.
uint32_t spl_store_oldest_seq(void);
uint32_t spl_store_next_seq(void);

/// Read the record with sequence number @p seq. False if not stored (anymore).
bool spl_store_read(uint32_t seq, spl_record_t *out);

/// Delete all records up to and including @p seq (BLE sync ACK).
void spl_store_delete_through(uint32_t seq);

/// Erase every record.
void spl_store_clear(void);

/// Call from the main loop: performs one increment of NVM3 repacking when
/// due, so appends never run into a monolithic multi-second repack.
void spl_store_maintain(void);

/// This boot's id and the watchdog reset counter maintained at boot.
uint32_t spl_store_boot_id(void);
uint32_t spl_store_watchdog_reset_count(void);

/// Extended-metric companion ring (B&K 2245 set), aligned to record seq.
/// 24-byte entries: 8 metrics in centi-dB after the 8-byte {boot_id, seq}.
#define SPL_STORE_MAX_EXTENDED 1000u
#define SPL_STORE_EXT_BATCH    4u

typedef struct __attribute__((packed)) {
  uint32_t boot_id;
  uint32_t seq;
  int16_t lafmin_cdb;
  int16_t lasmax_cdb;
  int16_t lasmin_cdb;
  int16_t lcpeak_cdb;
  int16_t lae_cdb;
  int16_t l10_cdb;
  int16_t l50_cdb;
  int16_t l90_cdb;
} spl_extended_t;

/// Append the extended metrics of the record with sequence number @p seq.
bool spl_store_append_extended(uint32_t seq, const spl_extended_t *ext);

/// Read the extended metrics for @p seq. False if not stored (or overwritten).
bool spl_store_read_extended(uint32_t seq, spl_extended_t *out);

/// 1/3-octave spectrum companion ring (aligned to record seq numbers).
/// 68-byte entries, 3 per NVM3 object; smaller than the base ring — spectra
/// are heavier, so they cover the most recent ~600 intervals only.
#define SPL_STORE_MAX_SPECTRA 600u
#define SPL_STORE_SPEC_BATCH  3u
#define SPL_STORE_SPEC_BANDS  30u

typedef struct __attribute__((packed)) {
  uint32_t boot_id;
  uint32_t seq;                              ///< Same seq as the base record
  int16_t bands_cdb[SPL_STORE_SPEC_BANDS];   ///< LZeq per band, centi-dB
} spl_spectrum_t;

/// Append the spectrum of the record with sequence number @p seq.
bool spl_store_append_spectrum(uint32_t seq, const int16_t bands_cdb[SPL_STORE_SPEC_BANDS]);

/// Read the spectrum for @p seq. False if not stored (or overwritten).
bool spl_store_read_spectrum(uint32_t seq, spl_spectrum_t *out);

/// Boot -> Unix epoch mapping, persisted on BLE time sync (protocolo_ble.md).
#define SPL_STORE_MAX_BOOT_EPOCHS 8u

typedef struct __attribute__((packed)) {
  uint32_t boot_id;
  uint32_t epoch_boot;   ///< Unix time (s) at uptime zero of that boot
} spl_boot_epoch_t;

/// Record/update the epoch of the current boot. Returns false on NVM3 error.
bool spl_store_set_boot_epoch(uint32_t epoch_boot);

/// Copy stored entries (newest kept) into @p out; returns entry count.
uint32_t spl_store_get_boot_epochs(spl_boot_epoch_t out[SPL_STORE_MAX_BOOT_EPOCHS]);

#endif // SPL_STORE_H
