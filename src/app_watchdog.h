/***************************************************************************//**
 * @file
 * @brief Watchdog supervision: WDOG0 + per-subsystem health tracking.
 *
 * WDOG0 (~2 s timeout) is fed from the main loop by app_watchdog_process(),
 * but only while every supervised subsystem is healthy:
 *  - audio: must kick periodically once its watch is enabled (freshness);
 *  - storage / BLE: sticky flags, set false on failure.
 * If anything wedges, the watchdog fires, the chip resets, and the boot code
 * reads the reset cause so the event can be counted in NVM3.
 ******************************************************************************/
#ifndef APP_WATCHDOG_H
#define APP_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

/// Audio must kick at least this often once enabled.
#define APP_WATCHDOG_AUDIO_FRESH_MS 500u

/// Read+latch reset cause, then start WDOG0 (~2 s, runs in EM2/EM3).
void app_watchdog_init(void);

/// Raw RSTCAUSE value latched at boot.
uint32_t app_watchdog_reset_cause(void);

/// True when the latched reset cause was a watchdog timeout.
bool app_watchdog_reset_was_watchdog(void);

/// Enable/disable supervision of the audio path (enabled in Fase 3).
void app_watchdog_audio_watch_enable(bool enable);

/// Audio path signals it processed a block (call from main context).
void app_watchdog_audio_kick(void);

/// Sticky health flags for storage and BLE.
void app_watchdog_set_storage_healthy(bool healthy);
void app_watchdog_set_ble_healthy(bool healthy);

/// Call every main-loop iteration: feeds WDOG0 while the system is healthy.
void app_watchdog_process(void);

#endif // APP_WATCHDOG_H
