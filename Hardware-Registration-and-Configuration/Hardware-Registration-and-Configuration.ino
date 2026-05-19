/*
    FireFly Hardware Registration and Configuration
    https://github.com/BrentIO/FireFly-Client

    Manufacturing-time application. Boots into WiFi SoftAP, exposes a REST API
    for writing device identity (UUID, product_id, product_hex, key) to EEPROM
    and optionally registering with FireFly-Cloud. No web UI. No LittleFS.
    WiFi credentials supplied by the operator are never persisted.
*/

#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <time.h>

extern "C" {
    void os_get_random(uint8_t *buf, size_t len);
}

#include "common/hardware.h"

#define HW_REG_APPLICATION "Hardware-Registration-and-Configuration"

static AsyncWebServer httpServer(80);

// Pending reboot: set after /api/mcu/reboot response is flushed
static unsigned long rebootAt = 0;

// Cloud registration state — updated by POST /api/registration
static bool registered = false;
static time_t registrationCheckedAt = 0;


// ─── Logging ─────────────────────────────────────────────────────────────────

#define LOG_CAPACITY 20

struct LogEntry {
    char         message[80];
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
    if (registrationCheckedAt > 0) {
        doc["checked_at"] = (uint32_t)registrationCheckedAt;
    } else {
        doc["checked_at"] = nullptr;
    }
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
    // TODO: FireFly-Cloud registration (HKDF-SHA-256 + ECDSA P-256 via BearSSL)
    req->send(501, "application/json", "{\"error\":\"cloud registration not yet implemented\"}");
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
        sta["interface"]   = "wifi_sta";
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

    doc["chip_model"]         = "ESP8266";
    snprintf(idBuf, sizeof(idBuf), "0x%06X", ESP.getChipId());
    doc["chip_id"]            = idBuf;
    doc["chip_cores"]         = 1;
    doc["sdk_version"]        = ESP.getSdkVersion();
    doc["cpu_freq_mhz"]       = ESP.getCpuFreqMHz();
    doc["flash_chip_size"]    = ESP.getFlashChipSize();
    doc["flash_chip_speed"]   = ESP.getFlashChipSpeed() / 1000000;
    doc["flash_chip_mode"]    = flashModeStr();
    char flashIdBuf[12];
    snprintf(flashIdBuf, sizeof(flashIdBuf), "0x%08X", ESP.getFlashChipId());
    doc["flash_chip_id"]      = flashIdBuf;
    doc["free_heap"]          = ESP.getFreeHeap();
    doc["max_free_block_size"]  = ESP.getMaxFreeBlockSize();
    doc["heap_fragmentation"] = ESP.getHeapFragmentation();
    doc["sketch_size"]        = ESP.getSketchSize();
    doc["free_sketch_space"]  = ESP.getFreeSketchSpace();
    doc["reset_reason"]       = ESP.getResetReason();

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
    req->send(200, "application/json", "{\"status\":\"rebooting\"}");
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
