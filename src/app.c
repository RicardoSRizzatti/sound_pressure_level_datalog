/***************************************************************************//**
 * @file
 * @brief SPL datalogger core application logic.
 *
 * Boot sequence: watchdog first (so a hang anywhere later still recovers),
 * then DSP, datalog, configuration, CLI, microphone streaming and the
 * measurement interval timer. The main loop processes microphone blocks,
 * closes measurement intervals and feeds the watchdog while healthy.
 ******************************************************************************/
#include "sl_bt_api.h"
#include "sl_main_init.h"
#include "app_assert.h"
#include "app_log.h"
#include "app.h"

#include "app_measurement.h"
#include "app_watchdog.h"
#include "mic_pdm.h"
#include "spl_ble.h"
#include "spl_cli.h"
#include "spl_config.h"
#include "spl_dsp.h"
#include "spl_store.h"

// The advertising set handle allocated from Bluetooth stack.
static uint8_t advertising_set_handle = 0xff;

// Application Init.
void app_init(void)
{
  app_watchdog_init();

  app_log_info("SPL datalogger booting (rstcause 0x%08lx%s)" APP_LOG_NL,
               (unsigned long)app_watchdog_reset_cause(),
               app_watchdog_reset_was_watchdog() ? ", WATCHDOG RESET" : "");

  spl_dsp_init();

  if (!spl_store_init()) {
    app_log_error("NVM3 datalog init failed" APP_LOG_NL);
    app_watchdog_set_storage_healthy(false);
  } else {
    app_log_info("boot %lu, %lu records stored, %lu watchdog resets" APP_LOG_NL,
                 (unsigned long)spl_store_boot_id(),
                 (unsigned long)spl_store_count(),
                 (unsigned long)spl_store_watchdog_reset_count());
  }

  spl_config_init();
  spl_cli_init();

  if (mic_pdm_start()) {
    app_log_info("microphone streaming at %u Hz" APP_LOG_NL,
                 (unsigned int)SPL_DSP_SAMPLE_RATE_HZ);
  } else {
    app_log_error("microphone start failed" APP_LOG_NL);
  }

  app_measurement_init();
}

// Application Process Action.
void app_process_action(void)
{
  if (app_is_process_required()) {
    mic_pdm_process();
    app_measurement_process();
    spl_ble_process();
  }
  app_watchdog_process();
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the default weak implementation.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  sl_status_t sc;

  spl_ble_on_event(evt);

  switch (SL_BT_MSG_ID(evt->header)) {
    // This event indicates the device has started and the radio is ready.
    case sl_bt_evt_system_boot_id:
      app_log_info("BLE stack booted, starting advertising" APP_LOG_NL);

      sc = sl_bt_advertiser_create_set(&advertising_set_handle);
      app_assert_status(sc);

      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      app_assert_status(sc);

      // Advertising interval: 100 ms (units of 0.625 ms).
      sc = sl_bt_advertiser_set_timing(advertising_set_handle,
                                       160, 160, 0, 0);
      app_assert_status(sc);

      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_legacy_advertiser_connectable);
      app_assert_status(sc);

      app_watchdog_set_ble_healthy(true);
      break;

    case sl_bt_evt_connection_opened_id:
      app_log_info("BLE connection opened" APP_LOG_NL);
      break;

    case sl_bt_evt_connection_closed_id:
      app_log_info("BLE connection closed, restarting advertising" APP_LOG_NL);
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      app_assert_status(sc);

      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_legacy_advertiser_connectable);
      app_assert_status(sc);
      break;

    default:
      break;
  }
}
