#pragma once

#define SUPPORTED_HARDWARE

/* FireFly Input — 6 inputs, 0 outputs, ESP8266, manufactured Nov 2020 */

/* Physical mapping of the LED GPIO pins in channel order (channels 1–6) */
#define LED_CHANNEL_PINS {5, 4, 14, 12, 13, 15}

#define LED_CHANNEL_COUNT 6
#define LED_PWM_MAX 1023  /* PWMRANGE on ESP8266 Arduino core */

#define FACTORY_RESET_PIN 10

#define PRODUCT_ID "FFI0600-2011"

inline void ledWrite(uint8_t pin, int value) {
    analogWrite(pin, value);
}
