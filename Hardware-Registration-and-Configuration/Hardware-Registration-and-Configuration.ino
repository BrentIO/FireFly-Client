/*
    FireFly Hardware Registration and Configuration
    https://github.com/BrentIO/FireFly-Client

    Manufacturing-time application. Boots into WiFi SoftAP, exposes a REST API
    for writing device identity (UUID, product_id, product_hex, key) to EEPROM
    and registering with FireFly-Cloud. No web UI. No LittleFS.
    WiFi credentials supplied by the operator are never persisted.
*/

/* ArduinoJson must be included before ESPAsyncWebServer so that ASYNC_JSON_SUPPORT
   is detected before AsyncJson.h is processed. */
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPAsyncWebServer.h>
#include <time.h>
#include <bearssl/bearssl_kdf.h>  // br_hkdf_* (includes bearssl_hash.h)
#include <bearssl/bearssl_ec.h>   // br_ec_compute_pub, br_ec_get_default

#include "common/hardware.h"

#ifndef FIREFLY_CLOUD_API_ROOT
    #error "FIREFLY_CLOUD_API_ROOT must be defined via build flags"
#endif

#ifdef __has_include
#  if __has_include("common/cloudCert.h")
#    include "common/cloudCert.h"
#  endif
#endif

#define HW_REG_APPLICATION "Hardware-Registration-and-Configuration"
#define DEVICE_CLASS       "CLIENT"

#ifndef VERSION
    #define VERSION "DEBUG"
#endif

#ifndef COMMIT_HASH
    #define COMMIT_HASH ""
#endif

static AsyncWebServer httpServer(80);

// Deferred reboot — set by POST /api/mcu/reboot after response is flushed
static unsigned long rebootAt = 0;

// Cloud registration state
static bool    registered            = false;
static time_t  registrationCheckedAt = 0;


// ─── Logging ─────────────────────────────────────────────────────────────────

#define LOG_CAPACITY 20

struct LogEntry {
    char          message[80];
    unsigned long timestamp;
};

static LogEntry eventLog[LOG_CAPACITY];
static int      eventHead  = 0;
static int      eventCount = 0;

static LogEntry errorLog[LOG_CAPACITY];
static int      errorHead  = 0;
static int      errorCount = 0;

static void pushLog(LogEntry* buf, int& head, int& count, const char* msg) {
    buf[head].timestamp = millis();
    strncpy(buf[head].message, msg, sizeof(buf[0].message) - 1);
    buf[head].message[sizeof(buf[0].message) - 1] = '\0';
    head = (head + 1) % LOG_CAPACITY;
    if (count < LOG_CAPACITY) count++;
}

static void addEvent(const char* msg) { pushLog(eventLog, eventHead, eventCount, msg); }
static void addError(const char* msg) { pushLog(errorLog, errorHead, errorCount, msg); }

static void appendLog(JsonArray arr, LogEntry* buf, int head, int count) {
    int start = (head - count + LOG_CAPACITY) % LOG_CAPACITY;
    for (int i = 0; i < count; i++) {
        JsonObject e = arr.add<JsonObject>();
        e["timestamp"] = buf[(start + i) % LOG_CAPACITY].timestamp;
        e["message"]   = buf[(start + i) % LOG_CAPACITY].message;
    }
}


// ─── Helpers ─────────────────────────────────────────────────────────────────

static String productHexStr() {
    char buf[12];
    snprintf(buf, sizeof(buf), "0x%08X", (uint32_t)PRODUCT_HEX);
    return String(buf);
}

static const char* flashModeStr() {
    switch (ESP.getFlashChipMode()) {
        case FM_QIO:   return "QIO";
        case FM_QOUT:  return "QOUT";
        case FM_DIO:   return "DIO";
        case FM_DOUT:  return "DOUT";
        default:       return "UNKNOWN";
    }
}

// Standard base64 (RFC 4648) with padding
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64Encode(const uint8_t* src, size_t srcLen, char* dst) {
    size_t i = 0, j = 0;
    while (i + 2 < srcLen) {
        dst[j++] = B64[(src[i] >> 2) & 0x3F];
        dst[j++] = B64[((src[i] & 0x3) << 4) | ((src[i+1] & 0xF0) >> 4)];
        dst[j++] = B64[((src[i+1] & 0xF) << 2) | ((src[i+2] & 0xC0) >> 6)];
        dst[j++] = B64[src[i+2] & 0x3F];
        i += 3;
    }
    if (i < srcLen) {
        dst[j++] = B64[(src[i] >> 2) & 0x3F];
        if (i + 1 < srcLen) {
            dst[j++] = B64[((src[i] & 0x3) << 4) | ((src[i+1] & 0xF0) >> 4)];
            dst[j++] = B64[((src[i+1] & 0xF) << 2)];
        } else {
            dst[j++] = B64[(src[i] & 0x3) << 4];
            dst[j++] = '=';
        }
        dst[j++] = '=';
    }
    dst[j] = '\0';
    return j;
}


// ─── Route handlers ──────────────────────────────────────────────────────────

static void handleGetIdentity(AsyncWebServerRequest* req) {
    if (!deviceIdentity.enabled) {
        req->send(404, "application/json", "{\"error\":\"not provisioned\"}");
        return;
    }
    JsonDocument doc;
    char hex[12];
    snprintf(hex, sizeof(hex), "0x%08X", deviceIdentity.data.product_hex);
    doc["uuid"]        = deviceIdentity.data.uuid;
    doc["product_id"]  = deviceIdentity.data.product_id;
    doc["product_hex"] = hex;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void handlePostIdentity(AsyncWebServerRequest* req, JsonVariant& body) {
    if (deviceIdentity.enabled) {
        req->send(409, "application/json", "{\"error\":\"already provisioned\"}");
        return;
    }
    if (!body.is<JsonObject>()) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    JsonObject obj = body.as<JsonObject>();

    const char* uuid       = obj["uuid"]        | "";
    const char* product_id = obj["product_id"]  | "";
    const char* hex_str    = obj["product_hex"] | "";

    if (strlen(uuid) != 36) {
        req->send(400, "application/json", "{\"error\":\"invalid uuid\"}");
        return;
    }
    if (strlen(product_id) == 0 || strlen(product_id) > 32) {
        req->send(400, "application/json", "{\"error\":\"invalid product_id\"}");
        return;
    }
    uint32_t reqHex = (uint32_t)strtoul(hex_str, nullptr, 16);
    if (reqHex != (uint32_t)PRODUCT_HEX) {
        req->send(400, "application/json", "{\"error\":\"product_hex mismatch\"}");
        return;
    }

    strncpy(deviceIdentity.data.uuid, uuid, sizeof(deviceIdentity.data.uuid) - 1);
    deviceIdentity.data.uuid[36] = '\0';
    strncpy(deviceIdentity.data.product_id, product_id, sizeof(deviceIdentity.data.product_id) - 1);
    deviceIdentity.data.product_id[32] = '\0';
    deviceIdentity.data.product_hex = reqHex;

    // Generate 32-byte master key on-device via hardware RNG
    os_get_random(deviceIdentity.data.key, sizeof(deviceIdentity.data.key));

    if (!deviceIdentity.write()) {
        addError("identity write() failed");
        req->send(500, "application/json", "{\"error\":\"write failed\"}");
        return;
    }

    addEvent("Identity written to EEPROM");
    req->send(201, "application/json", "{\"status\":\"ok\"}");
}

static void handleGetRegistration(AsyncWebServerRequest* req) {
    if (!deviceIdentity.enabled) {
        req->send(409, "application/json", "{\"error\":\"not provisioned\"}");
        return;
    }
    JsonDocument doc;
    doc["registered"] = registered;
    doc["checked_at"] = (long)registrationCheckedAt;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void handlePostRegistration(AsyncWebServerRequest* req, JsonVariant& body) {
    if (!deviceIdentity.enabled) {
        req->send(409, "application/json", "{\"error\":\"not provisioned\"}");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        req->send(503, "application/json", "{\"error\":\"no internet — POST /api/network/wifi first\"}");
        return;
    }
    if (!req->hasHeader("X-Registration-Key") ||
        req->header("X-Registration-Key").length() != 6) {
        req->send(400, "application/json",
                  "{\"error\":\"X-Registration-Key header is required and must be 6 characters\"}");
        return;
    }
    String regKey = req->header("X-Registration-Key");

    String cloudUrl = FIREFLY_CLOUD_API_ROOT;
    if (body.is<JsonObject>()) {
        const char* urlOverride = body["url"] | "";
        if (strlen(urlOverride) > 0) cloudUrl = urlOverride;
    }

    // Derive key_auth via HKDF-SHA-256 with info "firefly-auth-v1"
    uint8_t key_auth[32];
    br_hkdf_context hkdf;
    br_hkdf_init(&hkdf, &br_sha256_vtable, BR_HKDF_NO_SALT, 0);
    br_hkdf_inject(&hkdf, deviceIdentity.data.key, sizeof(deviceIdentity.data.key));
    br_hkdf_flip(&hkdf);
    br_hkdf_produce(&hkdf, "firefly-auth-v1", 15, key_auth, sizeof(key_auth));

    // Compute P-256 public key from key_auth
    br_ec_private_key sk;
    sk.curve = BR_EC_secp256r1;
    sk.x     = key_auth;
    sk.xlen  = sizeof(key_auth);

    uint8_t        pubKeyBuf[65];  // uncompressed: 0x04 || X (32) || Y (32)
    br_ec_public_key pk;
    size_t pubKeyLen = br_ec_compute_pub(br_ec_get_default(), &pk, pubKeyBuf, &sk);

    memset(key_auth, 0, sizeof(key_auth));  // clear derived key from RAM

    if (pubKeyLen == 0) {
        addError("EC public key computation failed");
        req->send(500, "application/json", "{\"error\":\"key computation failed\"}");
        return;
    }

    // Base64-encode the uncompressed public key
    char pubKeyB64[92];  // ceil(65/3)*4 + 1 = 92
    base64Encode(pubKeyBuf, pubKeyLen, pubKeyB64);

    // Build registration payload (matching Controller format, ESP8266 fields only)
    char hexStr[12];
    snprintf(hexStr, sizeof(hexStr), "0x%08X", deviceIdentity.data.product_hex);

    char chipIdStr[10];
    snprintf(chipIdStr, sizeof(chipIdStr), "0x%06X", ESP.getChipId());

    JsonDocument payload;
    payload["uuid"]                    = deviceIdentity.data.uuid;
    payload["product_id"]              = deviceIdentity.data.product_id;
    payload["product_hex"]             = hexStr;
    payload["device_class"]            = DEVICE_CLASS;
    payload["public_key"]              = pubKeyB64;
    payload["registering_application"] = HW_REG_APPLICATION;
    payload["registering_version"]     = VERSION;

    JsonObject mcu = payload["mcu"].to<JsonObject>();
    mcu["model"]            = "ESP8266";
    mcu["chip_id"]          = chipIdStr;
    mcu["cores"]            = 1;
    mcu["cpu_freq_mhz"]     = ESP.getCpuFreqMHz();
    mcu["flash_chip_size"]  = ESP.getFlashChipSize();
    mcu["flash_chip_speed"] = ESP.getFlashChipSpeed() / 1000000;
    mcu["flash_chip_mode"]  = flashModeStr();

    JsonArray network = payload["network"].to<JsonArray>();
    JsonObject netWifi = network.add<JsonObject>();
    netWifi["interface"]   = "wifi";
    netWifi["mac_address"] = WiFi.macAddress();
    JsonObject netAP = network.add<JsonObject>();
    netAP["interface"]   = "wifi_ap";
    netAP["mac_address"] = WiFi.softAPmacAddress();

    String payloadStr;
    serializeJson(payload, payloadStr);

    // POST to FireFly-Cloud
    WiFiClientSecure sslClient;
#ifdef FIREFLY_CLOUD_CERT_PEM
    static BearSSL::X509List cloudCA(FIREFLY_CLOUD_CERT_PEM);
    sslClient.setTrustAnchors(&cloudCA);
#else
    sslClient.setInsecure();
#endif

    HTTPClient http;
    String postUrl = cloudUrl + "/devices/register";
    http.begin(sslClient, postUrl);
    http.addHeader("Content-Type",       "application/json");
    http.addHeader("X-Registration-Key", regKey);
    http.setTimeout(10000);

    int status = http.POST(payloadStr);
    http.end();

    registrationCheckedAt = time(nullptr);

    if (status == 204) {
        registered = true;
        addEvent("Cloud registered");
        req->send(204);
    } else if (status == 401 || status == 403) {
        addError("Cloud reg rejected");
        req->send(403, "application/json", "{\"error\":\"invalid or expired registration key\"}");
    } else {
        addError("Cloud reg failed");
        JsonDocument errDoc;
        errDoc["error"] = "cloud registration failed";
        errDoc["status"] = status;
        String errOut;
        serializeJson(errDoc, errOut);
        req->send(502, "application/json", errOut);
    }
}

static void handlePostNetworkWifi(AsyncWebServerRequest* req, JsonVariant& body) {
    if (!body.is<JsonObject>()) {
        req->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    JsonObject obj = body.as<JsonObject>();

    const char* ssid     = obj["ssid"]     | "";
    const char* password = obj["password"] | "";

    if (strlen(ssid) == 0) {
        req->send(400, "application/json", "{\"error\":\"ssid required\"}");
        return;
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid, password);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
        delay(100);
    }

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect();
        WiFi.mode(WIFI_AP);
        addError("WiFi STA connect failed");
        req->send(503, "application/json", "{\"error\":\"connection failed\"}");
        return;
    }

    addEvent("WiFi STA connected");

    configTime(0, 0, "pool.ntp.org");
    unsigned long ntpStart = millis();
    while (time(nullptr) < 1000000000UL && millis() - ntpStart < 10000UL) {
        delay(100);
    }

    JsonDocument doc;
    doc["ip"] = WiFi.localIP().toString();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void handleGetNetwork(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    JsonObject ap = arr.add<JsonObject>();
    ap["interface"]   = "wifi_ap";
    ap["mac_address"] = WiFi.softAPmacAddress();
    ap["ip"]          = WiFi.softAPIP().toString();

    if (WiFi.status() == WL_CONNECTED) {
        JsonObject sta = arr.add<JsonObject>();
        sta["interface"]   = "wifi";
        sta["mac_address"] = WiFi.macAddress();
        sta["ip"]          = WiFi.localIP().toString();
    }

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void handleGetVersion(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["application"] = HW_REG_APPLICATION;
    doc["product_hex"] = productHexStr();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void handleGetMcu(AsyncWebServerRequest* req) {
    JsonDocument doc;
    char idBuf[10];

    doc["chip_model"]           = "ESP8266";
    snprintf(idBuf, sizeof(idBuf), "0x%06X", ESP.getChipId());
    doc["chip_id"]              = idBuf;
    doc["chip_cores"]           = 1;
    doc["sdk_version"]          = ESP.getSdkVersion();
    doc["cpu_freq_mhz"]         = ESP.getCpuFreqMHz();
    doc["flash_chip_size"]      = ESP.getFlashChipSize();
    doc["flash_chip_speed"]     = ESP.getFlashChipSpeed() / 1000000;
    doc["flash_chip_mode"]      = flashModeStr();
    char flashIdBuf[12];
    snprintf(flashIdBuf, sizeof(flashIdBuf), "0x%08X", ESP.getFlashChipId());
    doc["flash_chip_id"]        = flashIdBuf;
    doc["free_heap"]            = ESP.getFreeHeap();
    doc["max_free_block_size"]  = ESP.getMaxFreeBlockSize();
    doc["heap_fragmentation"]   = ESP.getHeapFragmentation();
    doc["sketch_size"]          = ESP.getSketchSize();
    doc["free_sketch_space"]    = ESP.getFreeSketchSpace();
    doc["reset_reason"]         = ESP.getResetReason();

    time_t now = time(nullptr);
    if (now > 1000000000UL) {
        doc["boot_time"] = (uint32_t)(now - millis() / 1000);
    } else {
        doc["boot_time"] = (uint32_t)(millis() / 1000);
    }

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void handlePostReboot(AsyncWebServerRequest* req) {
    req->send(204);
    rebootAt = millis() + 500;
}

static void handleGetPeripherals(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    JsonObject eeprom = arr.add<JsonObject>();
    eeprom["address"] = "0x00";
    eeprom["type"]    = "EEPROM";
    eeprom["online"]  = true;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void handleGetEvents(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    appendLog(arr, eventLog, eventHead, eventCount);
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void handleGetErrors(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    appendLog(arr, errorLog, errorHead, errorCount);
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}


// ─── Setup / Loop ─────────────────────────────────────────────────────────────

void setup() {

    // 1. LED pins — all off; no chase pattern or flash codes in HW-Reg
    for (int i = 0; i < LED_CHANNEL_COUNT; i++) {
        pinMode(LED_PINS[i], OUTPUT);
        ledWrite(LED_PINS[i], 0);
    }

    // 2. Read EEPROM identity — enabled=false is normal for an unprovisioned device
    deviceIdentity.begin();

    // 3. WiFi SoftAP — minimum TX power for short-range manufacturing use
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toUpperCase();
    String apSsid = String("FireFly-") + mac;

    WiFi.mode(WIFI_AP);
    WiFi.setOutputPower(0);
    WiFi.softAP(apSsid.c_str(), "", 1, false, 1);

    // 4. Register routes
    httpServer.on("/api/identity",     HTTP_GET,  handleGetIdentity);
    httpServer.on("/api/identity",     HTTP_POST, handlePostIdentity);
    httpServer.on("/api/registration", HTTP_GET,  handleGetRegistration);
    httpServer.on("/api/registration", HTTP_POST, handlePostRegistration);
    httpServer.on("/api/network/wifi", HTTP_POST, handlePostNetworkWifi);
    httpServer.on("/api/network",      HTTP_GET,  handleGetNetwork);
    httpServer.on("/api/version",      HTTP_GET,  handleGetVersion);
    httpServer.on("/api/mcu",          HTTP_GET,  handleGetMcu);
    httpServer.on("/api/mcu/reboot",   HTTP_POST, handlePostReboot);
    httpServer.on("/api/peripherals",  HTTP_GET,  handleGetPeripherals);
    httpServer.on("/api/events",       HTTP_GET,  handleGetEvents);
    httpServer.on("/api/errors",       HTTP_GET,  handleGetErrors);

    // 5. Start server
    httpServer.begin();
    addEvent("HW-Reg started");
}

void loop() {
    if (rebootAt > 0 && millis() >= rebootAt) {
        ESP.restart();
    }
}
