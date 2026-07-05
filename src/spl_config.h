/***************************************************************************//**
 * @file
 * @brief Persisted device configuration (metrics, interval, calibration).
 *
 * Written by the BLE Config characteristic or the bench CLI; stored in NVM3
 * so it survives resets (including watchdog resets).
 ******************************************************************************/
#ifndef SPL_CONFIG_H
#define SPL_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/// Metric selection bits.
#define SPL_METRIC_LAEQ    (1u << 0)
#define SPL_METRIC_LAFMAX  (1u << 1)

#define SPL_CONFIG_INTERVAL_MIN_S 5u
#define SPL_CONFIG_INTERVAL_MAX_S 3600u

typedef struct __attribute__((packed)) {
  uint8_t version;          ///< Layout version, bump on change
  uint8_t metrics;          ///< SPL_METRIC_* bitmask
  uint16_t interval_s;      ///< Measurement interval in seconds
  int16_t cal_offset_cdb;   ///< Calibration offset, centi-dB (dBFS -> dB SPL)
} spl_config_t;

/// Load from NVM3, falling back to defaults (LAeq+LAFmax, 20 s, +123.00 dB).
void spl_config_init(void);

/// Current configuration (in RAM).
const spl_config_t *spl_config_get(void);

/// Validate, persist to NVM3 and apply. Returns false if invalid or on
/// write failure (config still applied to RAM on write failure).
bool spl_config_set(const spl_config_t *cfg);

#endif // SPL_CONFIG_H
