#ifndef HARDWARE_H
#define HARDWARE_H

#ifdef PRODUCT_FFI0600_2011

    // FireFly Input — 6 inputs, 0 outputs, ESP8266, manufactured Nov 2020

    #define LED_CH1  5    // GPIO5  (PORT_A)
    #define LED_CH2  4    // GPIO4  (PORT_B)
    #define LED_CH3  14   // GPIO14 (PORT_C)
    #define LED_CH4  12   // GPIO12 (PORT_D)
    #define LED_CH5  13   // GPIO13 (PORT_E)
    #define LED_CH6  15   // GPIO15 (PORT_F)

    #define LED_CHANNEL_COUNT 6
    #define LED_PWM_MAX 1023  // PWMRANGE on ESP8266 Arduino core

    #define FACTORY_RESET_PIN 10

    #define PRODUCT_ID "FFI0600-2011"

    inline void ledWrite(uint8_t pin, int value) {
        analogWrite(pin, value);
    }

#else
    #error "No product defined — pass -DPRODUCT_FFI0600_2011 (or a future product define) to the compiler"
#endif

// LED channel index → GPIO pin mapping (populated at runtime from hardware definitions)
static const uint8_t LED_PINS[LED_CHANNEL_COUNT] = {
    LED_CH1, LED_CH2, LED_CH3, LED_CH4, LED_CH5, LED_CH6
};

#endif // HARDWARE_H
