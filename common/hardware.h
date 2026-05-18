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

/* Timing defaults — device headers may override any of these before this point */

#ifndef MQTT_RECONNECT_WAIT_MILLISECONDS
    #define MQTT_RECONNECT_WAIT_MILLISECONDS 5000UL
#endif

#ifndef WIFI_RECONNECT_INTERVAL_MS
    #define WIFI_RECONNECT_INTERVAL_MS 10000UL
#endif

#ifndef OTA_CHECK_INTERVAL_MS
    #define OTA_CHECK_INTERVAL_MS 86400000UL
#endif

#ifndef OTA_BOOT_DELAY_MS
    #define OTA_BOOT_DELAY_MS 30000UL
#endif

#ifndef PROVISIONING_SCAN_INTERVAL_MS
    #define PROVISIONING_SCAN_INTERVAL_MS 10000UL
#endif

#ifndef UNPROV_ROTATE_INTERVAL_MS
    #define UNPROV_ROTATE_INTERVAL_MS 200UL
#endif

/* LED channel index → GPIO pin mapping */
static const uint8_t LED_PINS[LED_CHANNEL_COUNT] = {
    LED_CH1, LED_CH2, LED_CH3, LED_CH4, LED_CH5, LED_CH6
};

#endif // hardware_definitions_h
