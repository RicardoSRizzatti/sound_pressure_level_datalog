/***************************************************************************//**
 * @file
 * @brief Custom BLE GATT service (contract: docs/protocolo_ble.md).
 *
 * Handles Config / Calibration / Time Sync / Boot Epochs / Status reads and
 * writes, and streams stored records to the app during a sync session with
 * ACK-based deletion.
 ******************************************************************************/
#ifndef SPL_BLE_H
#define SPL_BLE_H

#include "sl_bt_api.h"

/// Dispatch GATT events; call from sl_bt_on_event() before the default cases.
void spl_ble_on_event(sl_bt_msg_t *evt);

/// Call from the main loop: pushes pending record notifications.
void spl_ble_process(void);

/// Notify subscribers that a new record was appended (updates Status).
void spl_ble_notify_new_record(void);

#endif // SPL_BLE_H
