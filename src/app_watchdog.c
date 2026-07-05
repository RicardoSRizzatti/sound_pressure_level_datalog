/***************************************************************************//**
 * @file
 * @brief Watchdog supervision implementation.
 ******************************************************************************/
#include "app_watchdog.h"

#include "em_cmu.h"
#include "em_rmu.h"
#include "em_wdog.h"
#include "sl_sleeptimer.h"

static uint32_t reset_cause;

static bool audio_watch_enabled;
static uint32_t audio_last_kick_tick;
static bool storage_healthy = true;
static bool ble_healthy = true;

static void wdog_start(WDOG_PeriodSel_TypeDef period)
{
  WDOG_Init_TypeDef init = WDOG_INIT_DEFAULT;
  init.perSel = period;
  init.em2Run = true;
  init.em3Run = true;
  WDOGn_Enable(WDOG0, false);
  WDOGn_Init(WDOG0, &init);
}

void app_watchdog_init(void)
{
  reset_cause = RMU_ResetCauseGet();
  RMU_ResetCauseClear();

  // ULFRCO (~1 kHz) clocks WDOG0.
  CMU_ClockSelectSet(cmuClock_WDOG0CLK, cmuSelect_ULFRCO);
  CMU_ClockEnable(cmuClock_WDOG0, true);

  // Boot mode: ~64 s. NVM3 recovery/repack at init can block for several
  // seconds inside a single call — a 2 s period here causes a reset loop.
  wdog_start(wdogPeriod_64k);
}

void app_watchdog_arm_normal(void)
{
  wdog_start(wdogPeriod_2k);   // ~2 s supervision during normal operation
}

uint32_t app_watchdog_reset_cause(void)
{
  return reset_cause;
}

bool app_watchdog_reset_was_watchdog(void)
{
#ifdef EMU_RSTCAUSE_WDOG0
  return (reset_cause & EMU_RSTCAUSE_WDOG0) != 0;
#else
  return (reset_cause & RMU_RSTCAUSE_WDOGRST) != 0;
#endif
}

void app_watchdog_audio_watch_enable(bool enable)
{
  audio_watch_enabled = enable;
  if (enable) {
    audio_last_kick_tick = sl_sleeptimer_get_tick_count();
  }
}

void app_watchdog_audio_kick(void)
{
  audio_last_kick_tick = sl_sleeptimer_get_tick_count();
}

void app_watchdog_set_storage_healthy(bool healthy)
{
  storage_healthy = healthy;
}

void app_watchdog_set_ble_healthy(bool healthy)
{
  ble_healthy = healthy;
}

void app_watchdog_process(void)
{
  if (!storage_healthy || !ble_healthy) {
    return;
  }

  if (audio_watch_enabled) {
    uint32_t elapsed = sl_sleeptimer_get_tick_count() - audio_last_kick_tick;
    if (sl_sleeptimer_tick_to_ms(elapsed) > APP_WATCHDOG_AUDIO_FRESH_MS) {
      return;
    }
  }

  WDOGn_Feed(WDOG0);
}

void app_watchdog_feed_now(void)
{
  WDOGn_Feed(WDOG0);
}
