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

/* Device identity */
#ifdef ESP8266
    #include "deviceIdentityEEPROM.h"
#endif

/* Timing defaults — device headers may override any of these before this point */

#ifndef FLASH_ERROR_SHORT_MS
    #define FLASH_ERROR_SHORT_MS  500UL
#endif

#ifndef FLASH_ERROR_LONG_MS
    #define FLASH_ERROR_LONG_MS   1000UL
#endif

#ifndef FLASH_ERROR_GAP_MS
    #define FLASH_ERROR_GAP_MS    250UL
#endif

#ifndef FLASH_ERROR_PAUSE_MS
    #define FLASH_ERROR_PAUSE_MS  1000UL
#endif

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

/* Device identity instance */
#ifdef ESP8266
    managerDeviceIdentity deviceIdentity;
#endif

/* LED channel index → GPIO pin mapping; initializer list declared in device header as LED_CHANNEL_PINS */
static const uint8_t LED_PINS[LED_CHANNEL_COUNT] = LED_CHANNEL_PINS;

#endif // hardware_definitions_h
