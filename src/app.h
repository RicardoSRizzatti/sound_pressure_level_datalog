/***************************************************************************//**
 * @file
 * @brief SPL datalogger application interface (app_os_helper contract).
 ******************************************************************************/
#ifndef APP_H
#define APP_H

#include <stdbool.h>

/// Indicate that app_process_action() must run (safe from ISR context).
void app_proceed(void);

/// Check (and consume) a pending process request.
bool app_is_process_required(void);

/// Guard for shared state (no-op in bare-metal builds).
bool app_mutex_acquire(void);
void app_mutex_release(void);

/// Application runtime init used by app_os_helper.
void app_init_bt(void);

#endif // APP_H
