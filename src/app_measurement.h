/***************************************************************************//**
 * @file
 * @brief Measurement engine: closes SPL intervals on a periodic timer and
 *        appends the metrics to the NVM3 datalog.
 ******************************************************************************/
#ifndef APP_MEASUREMENT_H
#define APP_MEASUREMENT_H

#include <stdint.h>

/// Start the periodic interval timer using the persisted configuration.
void app_measurement_init(void);

/// Re-arm the timer after the configuration (interval) changed.
void app_measurement_apply_config(void);

/// Call from the main loop: closes the interval when the timer fired.
void app_measurement_process(void);

/// Seconds since boot.
uint32_t app_measurement_uptime_s(void);

#endif // APP_MEASUREMENT_H
