/***************************************************************************//**
 * @file
 * @brief User-feedback LED implementation.
 ******************************************************************************/
#include "app_led.h"

#include "sl_led.h"
#include "sl_simple_led_instances.h"
#include "sl_sleeptimer.h"

#define BLINK_ON_MS   120u
#define BLINK_OFF_MS  120u
#define BLINK_COUNT   3u

static sl_sleeptimer_timer_handle_t blink_timer;
static volatile uint8_t edges_left;   // on+off transitions remaining

static void on_edge(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;
  if (edges_left == 0) {
    sl_led_turn_off(&sl_led_led0);
    return;
  }
  edges_left--;
  bool turning_on = (edges_left % 2) == 1;   // sequence ends on OFF
  if (turning_on) {
    sl_led_turn_on(&sl_led_led0);
    (void)sl_sleeptimer_start_timer_ms(&blink_timer, BLINK_ON_MS, on_edge,
                                       NULL, 0, 0);
  } else {
    sl_led_turn_off(&sl_led_led0);
    if (edges_left > 0) {
      (void)sl_sleeptimer_start_timer_ms(&blink_timer, BLINK_OFF_MS, on_edge,
                                         NULL, 0, 0);
    }
  }
}

void app_led_blink_config_ack(void)
{
  // Restart cleanly if a previous blink is still running.
  (void)sl_sleeptimer_stop_timer(&blink_timer);
  edges_left = BLINK_COUNT * 2;   // N on-edges + N off-edges
  on_edge(NULL, NULL);
}
