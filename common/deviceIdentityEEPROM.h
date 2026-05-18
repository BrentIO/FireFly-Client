#pragma once

#ifndef ESP8266
    #error deviceIdentityEEPROM.h is only supported on ESP8266
#endif

#include <EEPROM.h>

#define DEVICE_IDENTITY_EEPROM_SIZE  54
#define DEVICE_IDENTITY_MAGIC_0      0x1E  /* ASCII Record Separator */
#define DEVICE_IDENTITY_MAGIC_1      0x04  /* ASCII End of Transmission */
#define DEVICE_IDENTITY_OFFSET_MAGIC  0
#define DEVICE_IDENTITY_OFFSET_UUID   2
#define DEVICE_IDENTITY_OFFSET_PHEX  18
#define DEVICE_IDENTITY_OFFSET_KEY   22

class managerDeviceIdentity {

    public:

        struct deviceType {
            char     uuid[37];       /**< RFC 4122 UUID string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\0" */
            char     product_id[33]; /**< Product ID string e.g. "FFI0600-2011\0" */
            uint8_t  key[32];        /**< 32-byte master secret (never transmitted) */
            uint32_t product_hex;    /**< Product hex code e.g. 0x06002011 */
        } data;

        bool enabled = false; /**< True if magic is valid and UUID is non-zero. */

        static void uuidBytesToString(const uint8_t bytes[16], char out[37]) {
            snprintf(out, 37,
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                bytes[0],  bytes[1],  bytes[2],  bytes[3],
                bytes[4],  bytes[5],
                bytes[6],  bytes[7],
                bytes[8],  bytes[9],
                bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
        }

        static bool uuidStringToBytes(const char* str, uint8_t out[16]) {
            if (strlen(str) != 36) return false;
            const char* src = str;
            int i = 0;
            const int groupLen[] = { 8, 4, 4, 4, 12 };
            for (int g = 0; g < 5; g++) {
                for (int j = 0; j < groupLen[g]; j += 2) {
                    char hex[3] = { src[j], src[j + 1], '\0' };
                    out[i++] = (uint8_t)strtoul(hex, NULL, 16);
                }
                src += groupLen[g];
                if (g < 4) src++; // skip dash
            }
            return true;
        }

        /**
         * Read identity from EEPROM. If magic bytes are absent or UUID is
         * all-zero, enabled is set false and no further action is taken.
         * If product_hex is present but does not match the compile-time
         * PRODUCT_HEX, the device halts in an infinite loop.
         * EEPROM is released (end()) before returning.
         */
        void begin() {
            EEPROM.begin(DEVICE_IDENTITY_EEPROM_SIZE);

            if (EEPROM.read(DEVICE_IDENTITY_OFFSET_MAGIC)     != DEVICE_IDENTITY_MAGIC_0 ||
                EEPROM.read(DEVICE_IDENTITY_OFFSET_MAGIC + 1) != DEVICE_IDENTITY_MAGIC_1) {
                EEPROM.end();
                enabled = false;
                return;
            }

            uint8_t uuidBytes[16];
            for (int i = 0; i < 16; i++) {
                uuidBytes[i] = EEPROM.read(DEVICE_IDENTITY_OFFSET_UUID + i);
            }

            bool hasUuid = false;
            for (int i = 0; i < 16; i++) {
                if (uuidBytes[i] != 0) { hasUuid = true; break; }
            }

            if (!hasUuid) {
                EEPROM.end();
                enabled = false;
                return;
            }

            uint32_t storedHex = 0;
            for (int i = 0; i < 4; i++) {
                storedHex |= ((uint32_t)EEPROM.read(DEVICE_IDENTITY_OFFSET_PHEX + i)) << (i * 8);
            }

            for (int i = 0; i < 32; i++) {
                data.key[i] = EEPROM.read(DEVICE_IDENTITY_OFFSET_KEY + i);
            }

            EEPROM.end();

            uuidBytesToString(uuidBytes, data.uuid);
            data.product_hex = storedHex;

            if (storedHex != (uint32_t)PRODUCT_HEX) {
                // Firmware built for a different hardware model — leave enabled=false
                // so setup() can call haltWithFlashCode() with the LEDs already init'd.
                return;
            }
            strlcpy(data.product_id, PRODUCT_ID, sizeof(data.product_id));
            enabled = true;
        }

        /**
         * Write identity to EEPROM. Returns false if already enabled (one-write
         * semantics: once written the identity cannot be overwritten).
         * Caller must populate data.uuid (as a string), data.product_hex, and
         * data.key before calling write().
         */
        bool write() {
            if (enabled) return false;

            uint8_t uuidBytes[16];
            if (!uuidStringToBytes(data.uuid, uuidBytes)) return false;

            EEPROM.begin(DEVICE_IDENTITY_EEPROM_SIZE);

            EEPROM.write(DEVICE_IDENTITY_OFFSET_MAGIC,     DEVICE_IDENTITY_MAGIC_0);
            EEPROM.write(DEVICE_IDENTITY_OFFSET_MAGIC + 1, DEVICE_IDENTITY_MAGIC_1);

            for (int i = 0; i < 16; i++) {
                EEPROM.write(DEVICE_IDENTITY_OFFSET_UUID + i, uuidBytes[i]);
            }

            for (int i = 0; i < 4; i++) {
                EEPROM.write(DEVICE_IDENTITY_OFFSET_PHEX + i, (data.product_hex >> (i * 8)) & 0xFF);
            }

            for (int i = 0; i < 32; i++) {
                EEPROM.write(DEVICE_IDENTITY_OFFSET_KEY + i, data.key[i]);
            }

            EEPROM.commit();
            EEPROM.end();

            strlcpy(data.product_id, PRODUCT_ID, sizeof(data.product_id));
            enabled = true;
            return true;
        }

        /**
         * Zero the entire identity block and clear the magic bytes.
         * Intended for use by the HW-Reg application only.
         * After wipe(), write() can be called to set a new identity.
         */
        void wipe() {
            EEPROM.begin(DEVICE_IDENTITY_EEPROM_SIZE);
            for (int i = 0; i < DEVICE_IDENTITY_EEPROM_SIZE; i++) {
                EEPROM.write(i, 0x00);
            }
            EEPROM.commit();
            EEPROM.end();
            enabled = false;
            memset(&data, 0, sizeof(data));
        }
};
