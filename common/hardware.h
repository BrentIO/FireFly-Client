/*
    Hardware Definitions
*/

#ifndef hardware_definitions_h
#define hardware_definitions_h

#define HARDWARE_CLASS "client"

#ifndef HARDWARE_MANUFACTURER_NAME
    #define HARDWARE_MANUFACTURER_NAME "P5 Software LLC"
#endif

/* Hardware Types */
#if PRODUCT_HEX == 0x06002011
    #include "devices/FFI0600-2011.h"
#endif

#ifndef SUPPORTED_HARDWARE
    #error "Build failed, Unknown PRODUCT_HEX. Ensure it was set in build flags: -DPRODUCT_HEX=0x06002011"
#endif

/* LED channel index → GPIO pin mapping */
static const uint8_t LED_PINS[LED_CHANNEL_COUNT] = {
    LED_CH1, LED_CH2, LED_CH3, LED_CH4, LED_CH5, LED_CH6
};

#endif // hardware_definitions_h
