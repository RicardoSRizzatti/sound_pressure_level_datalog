/***************************************************************************//**
 * @file
 * @brief Measurement engine implementation.
 ******************************************************************************/
#include "app_measurement.h"

#include "app.h"
#include "app_log.h"
#include "sl_sleeptimer.h"
#include "spl_ble.h"
#include "spl_config.h"
#include "spl_dsp.h"
#include "spl_store.h"
#include "spl_toct.h"

static sl_sleeptimer_timer_handle_t interval_timer;
static volatile bool interval_elapsed;

static void on_interval(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;
  interval_elapsed = true;
  app_proceed();
}

static void start_timer(void)
{
  (void)sl_sleeptimer_stop_timer(&interval_timer);
  uint32_t ms = (uint32_t)spl_config_get()->interval_s * 1000u;
  sl_status_t sc = sl_sleeptimer_start_periodic_timer_ms(
    &interval_timer, ms, on_interval, NULL, 0,
    SL_SLEEPTIMER_NO_HIGH_PRECISION_HF_CLOCKS_REQUIRED_FLAG);
  if (sc != SL_STATUS_OK) {
    app_log_error("interval timer start failed: 0x%lx" APP_LOG_NL,
                  (unsigned long)sc);
  }
}

void app_measurement_init(void)
{
  start_timer();
}

void app_measurement_apply_config(void)
{
  start_timer();
}

void app_measurement_process(void)
{
  if (!interval_elapsed) {
    return;
  }
  interval_elapsed = false;

  spl_result_t result;
  if (!spl_dsp_close_interval(&result)) {
    return;   // no audio accumulated (mic not running)
  }

  const spl_config_t *cfg = spl_config_get();
  int16_t laeq_cdb = (cfg->metrics & SPL_METRIC_LAEQ)
                     ? (int16_t)(result.laeq_db * 100.0f) : INT16_MIN;
  int16_t lafmax_cdb = (cfg->metrics & SPL_METRIC_LAFMAX)
                       ? (int16_t)(result.lafmax_db * 100.0f) : INT16_MIN;

  if (spl_store_append(laeq_cdb, lafmax_cdb, app_measurement_uptime_s())) {
    uint32_t seq = spl_store_next_seq() - 1;
    app_log_info("rec %lu: LAeq %d.%02d dB(A)  LAFmax %d.%02d dB(A)" APP_LOG_NL,
                 (unsigned long)seq,
                 laeq_cdb / 100, abs(laeq_cdb % 100),
                 lafmax_cdb / 100, abs(lafmax_cdb % 100));

    if (cfg->metrics & SPL_METRIC_SPECTRUM) {
      int16_t bands[SPL_TOCT_BANDS];
      if (spl_toct_close_interval(bands)
          && !spl_store_append_spectrum(seq, bands)) {
        app_log_error("NVM3 spectrum append failed" APP_LOG_NL);
      }
    }

    spl_ble_notify_new_record();
  } else {
    app_log_error("NVM3 append failed" APP_LOG_NL);
  }
}

uint32_t app_measurement_uptime_s(void)
{
  uint64_t ms;
  (void)sl_sleeptimer_tick64_to_ms(sl_sleeptimer_get_tick_count64(), &ms);
  return (uint32_t)(ms / 1000u);
}
