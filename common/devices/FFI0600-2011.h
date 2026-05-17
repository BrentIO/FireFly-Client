#pragma once

#define SUPPORTED_HARDWARE

/* FireFly Input — 6 inputs, 0 outputs, ESP8266, manufactured Nov 2020 */

#define LED_CH1  5    /* GPIO5  (PORT_A) */
#define LED_CH2  4    /* GPIO4  (PORT_B) */
#define LED_CH3  14   /* GPIO14 (PORT_C) */
#define LED_CH4  12   /* GPIO12 (PORT_D) */
#define LED_CH5  13   /* GPIO13 (PORT_E) */
#define LED_CH6  15   /* GPIO15 (PORT_F) */

#define LED_CHANNEL_COUNT 6
#define LED_PWM_MAX 1023  /* PWMRANGE on ESP8266 Arduino core */

#define FACTORY_RESET_PIN 10

#define PRODUCT_ID "FFI0600-2011"

inline void ledWrite(uint8_t pin, int value) {
    analogWrite(pin, value);
}
