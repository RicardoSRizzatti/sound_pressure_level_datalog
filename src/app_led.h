/***************************************************************************//**
 * @file
 * @brief User-feedback LED: non-blocking blink patterns on LED0.
 ******************************************************************************/
#ifndef APP_LED_H
#define APP_LED_H

/// Blink the on-board LED three times (config-accepted feedback). Driven by
/// a sleeptimer, so it never blocks the audio/BLE loop.
void app_led_blink_config_ack(void);

#endif // APP_LED_H
