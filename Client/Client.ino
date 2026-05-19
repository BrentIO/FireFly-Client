/*
    FireFly Client Firmware
    https://github.com/BrentIO/FireFly-Client
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#define MQTT_MAX_PACKET_SIZE 4096
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "common/hardware.h"

// ─── Firmware version ────────────────────────────────────────────────────────

#define APPLICATION_NAME "FireFly Client"

#ifndef VERSION
    #define VERSION "DEBUG"
#endif

#ifndef COMMIT_HASH
    #define COMMIT_HASH ""
#endif

// EXCESSIVE flash: 3 flashes, 100ms on / 100ms off — non-blocking
#define EXCESSIVE_FLASH_COUNT   3
#define EXCESSIVE_FLASH_ON_MS   100UL
#define EXCESSIVE_FLASH_OFF_MS  100UL

// LONG press: 100ms full-brightness flash, then hold at half retained (5% floor)
#define LONG_FLASH_ON_MS           100UL
#define LONG_HOLD_FLOOR_PCT        5

// ─── File paths ───────────────────────────────────────────────────────────────

#define CONFIG_FILE   "/config.json"
#define CERT_FILE     "/ca.pem"
#define CERT_FP_FILE  "/ca_fp.txt"

// ─── MQTT topic patterns ──────────────────────────────────────────────────────

#define MQTT_AVAILABILITY_TOPIC    "FireFly/%s/availability"
#define MQTT_TIME_START_TOPIC      "FireFly/%s/time-start/state"
#define MQTT_IP_ADDRESS_TOPIC      "FireFly/%s/ip-address/state"
#define MQTT_MAC_ADDRESS_TOPIC     "FireFly/%s/mac-address/state"
#define MQTT_COUNT_ERRORS_TOPIC    "FireFly/%s/count-errors/state"
#define MQTT_UPDATE_STATE_TOPIC    "FireFly/%s/update/state"
#define MQTT_UPDATE_SET_TOPIC      "FireFly/%s/update/set"
#define MQTT_INPUT_STATE_TOPIC     "FireFly/inputs/%s/channels/%d/state"
#define MQTT_LED_BRIGHTNESS_TOPIC  "FireFly/%s/leds/brightness/set"
#define MQTT_CERT_STATE_TOPIC      "FireFly/%s/cert/state"
#define MQTT_CERT_BROADCAST_TOPIC  "FireFly/clients/cert/state"

// ─── Data structures ──────────────────────────────────────────────────────────

struct LedChannel {
    uint8_t  pin;
    String   portId;
    int      channelNumber;

    // Retained current brightness (0–LED_PWM_MAX); authoritative idle level
    int      retainedBrightness;

    // Brightness saved before a transient button interaction
    int      savedBrightness;

    // EXCESSIVE flash state machine
    uint8_t       excessiveFlashStep;   // 0 = idle; 1–6 = on/off pairs
    unsigned long excessiveFlashAt;     // millis() when next step is due

    // LONG press state machine
    // longPressPhase: 0 = idle, 1 = flash phase, 2 = hold phase
    uint8_t       longPressPhase;
    unsigned long longFlashAt;          // millis() when flash phase ends
};

struct Config {
    String     id;
    String     name;
    String     area;
    String     wifiSsid;
    String     wifiPassword;
    String     mqttHost;
    int        mqttPort;
    String     mqttUsername;
    String     mqttPassword;
    String     otaUrl;
    LedChannel leds[LED_CHANNEL_COUNT];
    int        ledCount;
};

// ─── Globals ──────────────────────────────────────────────────────────────────

Config cfg;
bool   provisioned = false;

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

String certPem;
String certFingerprint;
String certCommonName;
String certOrganization;
String certExpiration;

int    errorCount = 0;
time_t bootTime   = 0;

unsigned long lastOtaCheck      = 0;
unsigned long lastMqttAttempt   = 0;
unsigned long lastWifiAttempt   = 0;
unsigned long lastProvScan      = 0;

// Unprovisioned LED rotation state
uint8_t       unProvRotateIndex = 0;
unsigned long unProvRotateAt    = 0;

// ─── Forward declarations ─────────────────────────────────────────────────────

void   haltWithFlashCode(uint8_t shortBursts, uint8_t longBursts);
void   runProvisioningMode();
bool   loadConfig();
bool   saveConfig(const String& json);
bool   loadCert();
void   connectWifi();
void   connectMqtt();
void   publishTelemetry();
void   publishAutoDiscovery();
void   mqttCallback(char* topic, byte* payload, unsigned int length);
void   handleInputState(const String& portId, int channel, const String& state);
void   handleBrightnessCommand(const String& payload);
void   handleCertBroadcast(const String& payload);
void   setLedRetained(int idx, int pwmValue);
int    percentToPwm(int pct);
void   tickExcessiveFlash(int idx);
void   tickLongPress(int idx);
void   rotateLeds();
void   checkOta();
String buildTopic(const char* pattern);

// ─── Setup ────────────────────────────────────────────────────────────────────

void setup() {

    deviceIdentity.begin();

    for (int i = 0; i < LED_CHANNEL_COUNT; i++) {
        pinMode(LED_PINS[i], OUTPUT);
        ledWrite(LED_PINS[i], 0);
    }

    if (!deviceIdentity.enabled) {
        haltWithFlashCode(3, 1);
    }

    pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);

    if (digitalRead(FACTORY_RESET_PIN) == LOW) {
        LittleFS.begin();
        LittleFS.remove(CONFIG_FILE);
        LittleFS.remove(CERT_FILE);
        LittleFS.remove(CERT_FP_FILE);
        LittleFS.end();
        ESP.restart();
    }

    if (!LittleFS.begin()) {
        // Mount failed — likely an incompatible filesystem format (e.g. SPIFFS from v1.14).
        // Format and remount so saveConfig() can write during provisioning.
        // Only done here: the path below where begin() succeeds but config is absent
        // must never format — the filesystem is healthy in that case.
        LittleFS.format();
        LittleFS.begin();
        runProvisioningMode();
        return;
    }

    provisioned = loadConfig();

    if (!provisioned) {
        runProvisioningMode();
        return;
    }

    loadCert();
    connectWifi();

    mqttClient.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
    mqttClient.setCallback(mqttCallback);
}

// ─── Main loop ────────────────────────────────────────────────────────────────

void loop() {

    if (!provisioned) {
        rotateLeds();
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastWifiAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
            connectWifi();
        }
        return;
    }

    if (!mqttClient.connected()) {
        if (millis() - lastMqttAttempt >= MQTT_RECONNECT_WAIT_MILLISECONDS) {
            connectMqtt();
        }
        return;
    }

    mqttClient.loop();

    unsigned long now = millis();
    if (bootTime > 0 && lastOtaCheck == 0 && now >= OTA_BOOT_DELAY_MS) {
        checkOta();
    } else if (lastOtaCheck > 0 && now - lastOtaCheck >= OTA_CHECK_INTERVAL_MS) {
        checkOta();
    }

    for (int i = 0; i < cfg.ledCount; i++) {
        if (cfg.leds[i].excessiveFlashStep > 0) {
            tickExcessiveFlash(i);
        }
        if (cfg.leds[i].longPressPhase > 0) {
            tickLongPress(i);
        }
    }
}

// ─── Provisioning ────────────────────────────────────────────────────────────

void runProvisioningMode() {

    // Scan for FireFly-Provisioning SoftAP and complete provisioning.
    // Password is derived from the Controller's BSSID via nibble-interleave algorithm.

    while (true) {

        rotateLeds();

        if (millis() - lastProvScan < PROVISIONING_SCAN_INTERVAL_MS) {
            continue;
        }
        lastProvScan = millis();

        int nets = WiFi.scanNetworks();
        for (int n = 0; n < nets; n++) {
            if (WiFi.SSID(n) != "FireFly-Provisioning") continue;

            uint8_t bssid[6];
            memcpy(bssid, WiFi.BSSID(n), 6);
            char password[13];
            for (int i = 0; i < 6; i++) {
                uint8_t upper = (bssid[i] >> 4) & 0x0F;
                uint8_t lower = (bssid[5 - i]) & 0x0F;
                snprintf(&password[i * 2], 3, "%X%X", upper, lower);
            }

            WiFi.begin("FireFly-Provisioning", (const char*)password);
            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
                rotateLeds();
                delay(50);
            }

            if (WiFi.status() != WL_CONNECTED) break;

            WiFiClient client;
            HTTPClient  http;

            // Fetch nonce
            http.begin(client, "http://192.168.4.1/api/provisioning/nonce");
            int code = http.GET();
            if (code != 200) { http.end(); break; }
            String nonce = http.getString();
            http.end();
            nonce.trim();

            // Fetch config
            http.begin(client, "http://192.168.4.1/api/provisioning/client");
            http.addHeader("mac-address", WiFi.macAddress());
            http.addHeader("x-nonce", nonce);
            code = http.GET();
            if (code != 200) { http.end(); break; }
            String configJson = http.getString();
            http.end();

            if (!saveConfig(configJson)) break;

            // Fetch CA certificate — macAddress auth only (cert is not sensitive)
            http.begin(client, "http://192.168.4.1/api/provisioning/certs");
            http.addHeader("mac-address", WiFi.macAddress());
            code = http.GET();
            if (code == 200) {
                String certJson = http.getString();
                JsonDocument doc;
                if (deserializeJson(doc, certJson) == DeserializationError::Ok) {
                    String fp  = doc["fingerprint"].as<String>();
                    String pem = doc["pem"].as<String>();
                    if (fp.length() > 0 && pem.length() > 0) {
                        File f = LittleFS.open(CERT_FILE, "w");
                        if (f) { f.print(pem); f.close(); }
                        File ff = LittleFS.open(CERT_FP_FILE, "w");
                        if (ff) { ff.print(fp); ff.close(); }
                    }
                }
            }
            http.end();

            WiFi.disconnect();
            delay(500);
            ESP.restart();
        }
    }
}

// ─── Config ───────────────────────────────────────────────────────────────────

bool loadConfig() {

    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) return false;

    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return false; }
    f.close();

    cfg.id           = doc["id"].as<String>();
    cfg.name         = doc["name"].as<String>();
    cfg.area         = doc["area"].as<String>();
    cfg.wifiSsid     = doc["wifi"]["ssid"].as<String>();
    cfg.wifiPassword = doc["wifi"]["password"].as<String>();
    cfg.mqttHost     = doc["mqtt"]["host"].as<String>();
    cfg.mqttPort     = doc["mqtt"]["port"] | 1883;
    cfg.mqttUsername = doc["mqtt"]["username"].as<String>();
    cfg.mqttPassword = doc["mqtt"]["password"].as<String>();
    cfg.otaUrl       = doc["ota"]["url"].as<String>();

    // Initialize all slots; unconfigured slots stay with empty portId
    for (int i = 0; i < LED_CHANNEL_COUNT; i++) {
        cfg.leds[i].pin = LED_PINS[i];
        cfg.leds[i].portId = "";
        cfg.leds[i].channelNumber = 0;
        cfg.leds[i].retainedBrightness = 0;
        cfg.leds[i].savedBrightness = 0;
        cfg.leds[i].excessiveFlashStep = 0;
        cfg.leds[i].longPressPhase = 0;
    }

    // id is a root-level field shared by all HIDs on this client (same port on the controller).
    String portId = doc["id"].as<String>();

    // Hids are keyed 1–6 (1-indexed); the key IS the channelNumber in the MQTT topic.
    // Iterate by explicit key so HID numbering is stable regardless of inversion or count.
    cfg.ledCount = 0;
    JsonObject hids = doc["hids"].as<JsonObject>();
    for (int ch = 1; ch <= LED_CHANNEL_COUNT; ch++) {
        JsonObject hid = hids[String(ch)];
        if (hid.isNull()) continue;
        if (portId.length() == 0) continue;
        int idx = ch - 1;  // 0-based index into LED_PINS[]
        cfg.leds[idx].portId             = portId;
        cfg.leds[idx].channelNumber      = ch;
        cfg.leds[idx].retainedBrightness = percentToPwm(hid["defaultBrightness"] | 100);
        cfg.leds[idx].savedBrightness    = cfg.leds[idx].retainedBrightness;
        cfg.ledCount = ch;
    }

    return cfg.wifiSsid.length() > 0;
}

bool saveConfig(const String& json) {
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) return false;
    f.print(json);
    f.close();
    return true;
}

static String dnField(const uint8_t* dn, size_t len, uint8_t oidFinal) {
    // Scan for DER OID pattern {0x06, 0x03, 0x55, 0x04, oidFinal} and extract the value.
    for (size_t i = 0; i + 6 < len; i++) {
        if (dn[i]   != 0x06) continue;
        if (dn[i+1] != 0x03) continue;
        if (dn[i+2] != 0x55) continue;
        if (dn[i+3] != 0x04) continue;
        if (dn[i+4] != oidFinal) continue;
        uint8_t tag = dn[i+5];
        if (i + 7 >= len) break;
        uint8_t vlen = dn[i+6];
        if (vlen & 0x80) break;  // long-form length; shouldn't occur in 2.5.4.x
        if (i + 7 + vlen > len) break;
        const uint8_t* vp = dn + i + 7;
        if (tag == 0x1E) {
            // BMPString (UTF-16BE): extract ASCII bytes at odd offsets
            String s;
            for (uint8_t k = 1; k < vlen; k += 2) s += (char)vp[k];
            return s;
        }
        if (tag == 0x0C || tag == 0x13 || tag == 0x16) {
            return String((const char*)vp).substring(0, vlen);
        }
    }
    return String();
}

struct PemDerCtx {
    uint8_t data[2048];
    size_t  len;
};

static void pemDerAppend(void* ctx, const void* src, size_t len) {
    PemDerCtx* p = (PemDerCtx*)ctx;
    if (p->len + len > sizeof(p->data)) len = sizeof(p->data) - p->len;
    memcpy(p->data + p->len, src, len);
    p->len += len;
}

struct DnCtx {
    uint8_t data[512];
    size_t  len;
};

static void dnAppend(void* ctx, const void* src, size_t len) {
    DnCtx* d = (DnCtx*)ctx;
    if (d->len + len > sizeof(d->data)) len = sizeof(d->data) - d->len;
    memcpy(d->data + d->len, src, len);
    d->len += len;
}

static void parseCertMetadata(const String& pem) {
    certCommonName   = String();
    certOrganization = String();
    certExpiration   = String();

    PemDerCtx der;
    der.len = 0;

    br_pem_decoder_context pemCtx;
    br_pem_decoder_init(&pemCtx);
    br_pem_decoder_setdest(&pemCtx, pemDerAppend, &der);

    const char* src = pem.c_str();
    size_t remaining = pem.length();
    bool done = false;
    while (remaining > 0 && !done) {
        size_t consumed = br_pem_decoder_push(&pemCtx, src, remaining);
        src       += consumed;
        remaining -= consumed;
        int event = br_pem_decoder_event(&pemCtx);
        if (event == BR_PEM_END_OBJ) done = true;
        if (event == BR_PEM_ERROR)   return;
    }

    if (der.len == 0) return;

    DnCtx dn;
    dn.len = 0;

    br_x509_decoder_context x509;
    br_x509_decoder_init(&x509, dnAppend, &dn);
    br_x509_decoder_push(&x509, der.data, der.len);

    uint32_t days    = x509.notafter_days;
    uint32_t seconds = x509.notafter_seconds;
    if (days > 0) {
        time_t t = ((int64_t)days - 719162LL) * 86400LL + seconds;
        struct tm* gt = gmtime(&t);
        char buf[11];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", gt->tm_year + 1900, gt->tm_mon + 1, gt->tm_mday);
        certExpiration = String(buf);
    }

    certCommonName   = dnField(dn.data, dn.len, 0x03);
    certOrganization = dnField(dn.data, dn.len, 0x0A);
}

bool loadCert() {
    File f = LittleFS.open(CERT_FILE, "r");
    if (!f) return false;
    certPem = f.readString();
    f.close();

    File ff = LittleFS.open(CERT_FP_FILE, "r");
    if (ff) {
        certFingerprint = ff.readString();
        certFingerprint.trim();
        ff.close();
    }

    parseCertMetadata(certPem);

    return certPem.length() > 0;
}

// ─── WiFi ─────────────────────────────────────────────────────────────────────

void connectWifi() {
    lastWifiAttempt = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPassword.c_str());
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────

void connectMqtt() {

    lastMqttAttempt = millis();

    String availTopic = buildTopic(MQTT_AVAILABILITY_TOPIC);
    bool connected = mqttClient.connect(
        deviceIdentity.data.uuid,
        cfg.mqttUsername.c_str(),
        cfg.mqttPassword.c_str(),
        availTopic.c_str(), 0, true, "offline"
    );

    if (!connected) return;

    mqttClient.publish(availTopic.c_str(), "online", true);

    // Subscribe to input state topics for each configured HID channel
    char topic[128];
    for (int i = 0; i < cfg.ledCount; i++) {
        if (cfg.leds[i].portId.length() == 0) continue;
        snprintf(topic, sizeof(topic), MQTT_INPUT_STATE_TOPIC,
                 cfg.leds[i].portId.c_str(), cfg.leds[i].channelNumber);
        mqttClient.subscribe(topic);
    }

    mqttClient.subscribe(buildTopic(MQTT_LED_BRIGHTNESS_TOPIC).c_str());
    mqttClient.subscribe(buildTopic(MQTT_UPDATE_SET_TOPIC).c_str());
    mqttClient.subscribe(MQTT_CERT_BROADCAST_TOPIC);

    publishTelemetry();
    publishAutoDiscovery();

    if (bootTime == 0) {
        bootTime = millis() / 1000;
    }

    for (int i = 0; i < cfg.ledCount; i++) {
        if (cfg.leds[i].portId.length() == 0) continue;
        ledWrite(cfg.leds[i].pin, cfg.leds[i].retainedBrightness);
    }
}

// ─── MQTT callback ───────────────────────────────────────────────────────────

void mqttCallback(char* topic, byte* payload, unsigned int length) {

    String t(topic);
    String p;
    p.reserve(length);
    for (unsigned int i = 0; i < length; i++) p += (char)payload[i];
    p.trim();

    // Input state: FireFly/inputs/{portId}/channels/{channelNumber}/state
    if (t.startsWith("FireFly/inputs/") && t.endsWith("/state")) {
        int chansIdx = t.indexOf("/channels/");
        if (chansIdx < 0) return;
        String portId = t.substring(15, chansIdx);
        String rest   = t.substring(chansIdx + 10);
        int slashIdx  = rest.indexOf('/');
        if (slashIdx < 0) return;
        int channelNumber = rest.substring(0, slashIdx).toInt();
        handleInputState(portId, channelNumber, p);
        return;
    }

    if (t == buildTopic(MQTT_LED_BRIGHTNESS_TOPIC)) {
        handleBrightnessCommand(p);
        return;
    }

    if (t == buildTopic(MQTT_UPDATE_SET_TOPIC)) {
        if (p == "do-update") checkOta();
        return;
    }

    if (t == MQTT_CERT_BROADCAST_TOPIC) {
        handleCertBroadcast(p);
        return;
    }
}

// ─── LED: input state handling ───────────────────────────────────────────────

void handleInputState(const String& portId, int channelNumber, const String& state) {

    for (int i = 0; i < cfg.ledCount; i++) {
        if (cfg.leds[i].portId != portId || cfg.leds[i].channelNumber != channelNumber) continue;

        if (state == "NORMAL") {
            cfg.leds[i].excessiveFlashStep = 0;
            cfg.leds[i].longPressPhase     = 0;
            ledWrite(cfg.leds[i].pin, cfg.leds[i].retainedBrightness);

        } else if (state == "SHORT") {
            cfg.leds[i].savedBrightness = cfg.leds[i].retainedBrightness;
            ledWrite(cfg.leds[i].pin, 0);

        } else if (state == "LONG") {
            cfg.leds[i].savedBrightness  = cfg.leds[i].retainedBrightness;
            cfg.leds[i].longPressPhase   = 1;
            cfg.leds[i].longFlashAt      = millis() + LONG_FLASH_ON_MS;
            ledWrite(cfg.leds[i].pin, LED_PWM_MAX);

        } else if (state == "EXCESSIVE") {
            cfg.leds[i].savedBrightness    = cfg.leds[i].retainedBrightness;
            cfg.leds[i].excessiveFlashStep = 1;
            cfg.leds[i].excessiveFlashAt   = millis();
            ledWrite(cfg.leds[i].pin, LED_PWM_MAX);
        }
        return;
    }
}

// ─── LED: EXCESSIVE flash state machine ──────────────────────────────────────

void tickExcessiveFlash(int idx) {

    LedChannel& led = cfg.leds[idx];
    if (millis() < led.excessiveFlashAt) return;

    // Odd steps = ON phase ending (go OFF); even steps = OFF phase ending (go ON or finish)
    bool isOnPhase = (led.excessiveFlashStep % 2 == 1);

    if (isOnPhase) {
        ledWrite(led.pin, 0);
        led.excessiveFlashAt = millis() + EXCESSIVE_FLASH_OFF_MS;
        led.excessiveFlashStep++;
    } else {
        if (led.excessiveFlashStep >= EXCESSIVE_FLASH_COUNT * 2) {
            led.excessiveFlashStep = 0;
            ledWrite(led.pin, led.retainedBrightness);
        } else {
            ledWrite(led.pin, LED_PWM_MAX);
            led.excessiveFlashAt = millis() + EXCESSIVE_FLASH_ON_MS;
            led.excessiveFlashStep++;
        }
    }
}

// ─── LED: LONG press state machine ───────────────────────────────────────────

void tickLongPress(int idx) {
    LedChannel& led = cfg.leds[idx];
    if (led.longPressPhase != 1) return;
    if (millis() < led.longFlashAt) return;

    int holdLevel = led.savedBrightness / 2;
    int floor     = percentToPwm(LONG_HOLD_FLOOR_PCT);
    if (holdLevel < floor) holdLevel = floor;

    led.longPressPhase = 2;
    ledWrite(led.pin, holdLevel);
}

// ─── LED: brightness commands ────────────────────────────────────────────────

// Payload is a numeric brightness percentage (0–100), applied to all configured LEDs
void handleBrightnessCommand(const String& payload) {
    int pct = payload.toInt();
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    int pwm = percentToPwm(pct);
    for (int i = 0; i < cfg.ledCount; i++) {
        if (cfg.leds[i].portId.length() == 0) continue;
        setLedRetained(i, pwm);
    }
}

void setLedRetained(int idx, int pwmValue) {
    cfg.leds[idx].retainedBrightness = pwmValue;
    if (cfg.leds[idx].excessiveFlashStep == 0) {
        ledWrite(cfg.leds[idx].pin, pwmValue);
    }
}

// ─── LED: fatal flash code (never returns) ───────────────────────────────────

void haltWithFlashCode(uint8_t shortBursts, uint8_t longBursts) {
    while (true) {
        for (uint8_t s = 0; s < shortBursts; s++) {
            for (int i = 0; i < LED_CHANNEL_COUNT; i++) ledWrite(LED_PINS[i], LED_PWM_MAX);
            delay(FLASH_ERROR_SHORT_MS);
            for (int i = 0; i < LED_CHANNEL_COUNT; i++) ledWrite(LED_PINS[i], 0);
            if (s < shortBursts - 1) delay(FLASH_ERROR_GAP_MS);
        }
        delay(FLASH_ERROR_PAUSE_MS);
        for (uint8_t l = 0; l < longBursts; l++) {
            for (int i = 0; i < LED_CHANNEL_COUNT; i++) ledWrite(LED_PINS[i], LED_PWM_MAX);
            delay(FLASH_ERROR_LONG_MS);
            for (int i = 0; i < LED_CHANNEL_COUNT; i++) ledWrite(LED_PINS[i], 0);
            if (l < longBursts - 1) delay(FLASH_ERROR_GAP_MS);
        }
        delay(FLASH_ERROR_PAUSE_MS);
    }
}

// ─── LED: unprovisioned rotation ─────────────────────────────────────────────

void rotateLeds() {
    if (millis() < unProvRotateAt) return;
    unProvRotateAt = millis() + UNPROV_ROTATE_INTERVAL_MS;
    for (int i = 0; i < LED_CHANNEL_COUNT; i++) {
        ledWrite(LED_PINS[i], (i == unProvRotateIndex) ? LED_PWM_MAX : 0);
    }
    unProvRotateIndex = (unProvRotateIndex + 1) % LED_CHANNEL_COUNT;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

int percentToPwm(int pct) {
    if (pct <= 0)   return 0;
    if (pct >= 100) return LED_PWM_MAX;
    return (int)((float)pct / 100.0f * LED_PWM_MAX);
}

String buildTopic(const char* pattern) {
    char buf[128];
    snprintf(buf, sizeof(buf), pattern, deviceIdentity.data.uuid);
    return String(buf);
}

// ─── Telemetry ───────────────────────────────────────────────────────────────

void publishTelemetry() {
    mqttClient.publish(buildTopic(MQTT_IP_ADDRESS_TOPIC).c_str(),  WiFi.localIP().toString().c_str(), true);
    mqttClient.publish(buildTopic(MQTT_MAC_ADDRESS_TOPIC).c_str(), WiFi.macAddress().c_str(), true);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", errorCount);
    mqttClient.publish(buildTopic(MQTT_COUNT_ERRORS_TOPIC).c_str(), buf, true);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)bootTime);
    mqttClient.publish(buildTopic(MQTT_TIME_START_TOPIC).c_str(), buf, true);
    if (certFingerprint.length() > 0) {
        mqttClient.publish(buildTopic(MQTT_CERT_STATE_TOPIC).c_str(), certFingerprint.c_str(), true);
    }
}

// ─── HA Autodiscovery ────────────────────────────────────────────────────────

void publishAutoDiscovery() {

    String uuid  = String(deviceIdentity.data.uuid);
    String avail = buildTopic(MQTT_AVAILABILITY_TOPIC);

    auto addDevice = [&](JsonDocument& doc) {
        JsonObject device = doc["device"].to<JsonObject>();
        JsonArray  ids    = device["identifiers"].to<JsonArray>();
        ids.add(uuid);
        device["name"]          = cfg.name;
        device["manufacturer"]  = HARDWARE_MANUFACTURER_NAME;
        device["model"]         = APPLICATION_NAME;
        device["model_id"]      = PRODUCT_ID;
        device["serial_number"] = uuid;
        device["sw_version"]    = VERSION;
        if (cfg.area.length() > 0)         device["suggested_area"]   = cfg.area;
        if (certCommonName.length() > 0)   device["cert_common_name"] = certCommonName;
        if (certOrganization.length() > 0) device["cert_organization"]= certOrganization;
        if (certExpiration.length() > 0)   device["cert_expiration"]  = certExpiration;
    };

    auto publishSensor = [&](const char* id, const char* name, const char* stateTopic, const char* icon) {
        String discTopic = "homeassistant/sensor/FireFly-" + uuid + "-" + id + "/config";
        JsonDocument doc;
        doc["unique_id"]          = "FireFly-" + uuid + "-" + id;
        doc["name"]               = name;
        doc["state_topic"]        = stateTopic;
        doc["availability_topic"] = avail;
        if (icon) doc["icon"]     = icon;
        addDevice(doc);
        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(discTopic.c_str(), payload.c_str(), true);
    };

    publishSensor("ip-address",  "IP Address",             buildTopic(MQTT_IP_ADDRESS_TOPIC).c_str(),  "mdi:ip");
    publishSensor("mac-address", "MAC Address",            buildTopic(MQTT_MAC_ADDRESS_TOPIC).c_str(), "mdi:ethernet");
    publishSensor("time-start",  "Boot Time",              buildTopic(MQTT_TIME_START_TOPIC).c_str(),  "mdi:clock-start");
    publishSensor("count-errors","Error Count",            buildTopic(MQTT_COUNT_ERRORS_TOPIC).c_str(),"mdi:alert-circle");
    publishSensor("cert",        "Certificate Fingerprint",buildTopic(MQTT_CERT_STATE_TOPIC).c_str(),  "mdi:certificate");

    // OTA update entity
    {
        String discTopic = "homeassistant/update/FireFly-" + uuid + "-update/config";
        JsonDocument doc;
        doc["unique_id"]          = "FireFly-" + uuid + "-update";
        doc["name"]               = "Firmware Update";
        doc["state_topic"]        = buildTopic(MQTT_UPDATE_STATE_TOPIC);
        doc["command_topic"]      = buildTopic(MQTT_UPDATE_SET_TOPIC);
        doc["payload_install"]    = "do-update";
        doc["availability_topic"] = avail;
        addDevice(doc);
        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(discTopic.c_str(), payload.c_str(), true);
    }
}

// ─── OTA ─────────────────────────────────────────────────────────────────────

void checkOta() {
    lastOtaCheck = millis();

    if (cfg.otaUrl.length() == 0) return;

    String url = cfg.otaUrl;
    char hexStr[16];
    snprintf(hexStr, sizeof(hexStr), "0x%08X", (uint32_t)PRODUCT_HEX);
    url.replace("$$hex$$", hexStr);
    url.replace("$$class$$", HARDWARE_CLASS);
    url += "?current_version=";
    url += VERSION;

    WiFiClientSecure checkClient;
    BearSSL::X509List checkCA;
    if (certPem.length() > 0) {
        checkCA.append(certPem.c_str());
        checkClient.setTrustAnchors(&checkCA);
    } else {
        checkClient.setInsecure();
    }

    HTTPClient http;
    http.begin(checkClient, url);
    int code = http.GET();

    if (code != 200) { http.end(); return; }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return;

    String latestVersion = doc["version"].as<String>();
    String firmwareUrl   = doc["url"].as<String>();
    String title         = doc["title"].as<String>();
    String releaseUrl    = doc["release_url"].as<String>();

    if (latestVersion == VERSION || firmwareUrl.length() == 0) return;

    auto publishState = [&](bool inProgress, int pct) {
        JsonDocument stateDoc;
        stateDoc["installed_version"] = VERSION;
        stateDoc["latest_version"]    = latestVersion;
        stateDoc["in_progress"]       = inProgress;
        stateDoc["update_percentage"] = pct;
        if (title.length() > 0)      stateDoc["title"]       = title;
        if (releaseUrl.length() > 0) stateDoc["release_url"] = releaseUrl;
        String statePayload;
        serializeJson(stateDoc, statePayload);
        mqttClient.publish(buildTopic(MQTT_UPDATE_STATE_TOPIC).c_str(), statePayload.c_str(), true);
    };

    publishState(false, 0);

    ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);
    ESPhttpUpdate.onProgress([&](int cur, int total) {
        if (total > 0) publishState(true, (cur * 100) / total);
    });

    WiFiClientSecure updateClient;
    BearSSL::X509List updateCA;
    if (certPem.length() > 0) {
        updateCA.append(certPem.c_str());
        updateClient.setTrustAnchors(&updateCA);
    } else {
        updateClient.setInsecure();
    }

    t_httpUpdate_return result = ESPhttpUpdate.update(updateClient, firmwareUrl);

    if (result == HTTP_UPDATE_FAILED) {
        errorCount++;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", errorCount);
        mqttClient.publish(buildTopic(MQTT_COUNT_ERRORS_TOPIC).c_str(), buf, true);
    }
    // HTTP_UPDATE_OK causes ESP.restart() internally — never reaches here
}

// ─── Cert rotation ────────────────────────────────────────────────────────────

// Handles the retained broadcast on FireFly/clients/cert/state.
// Payload is JSON: {"fingerprint":"<sha256>","pem":"<PEM string>"}.
// All ESP8266 clients subscribe to this topic; offline devices pick up the
// retained message on reconnect, ensuring cert rotation reaches every device.
void handleCertBroadcast(const String& payload) {
    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) return;

    String fp  = doc["fingerprint"].as<String>();
    String pem = doc["pem"].as<String>();

    if (fp.length() == 0 || pem.length() == 0) return;
    if (fp == certFingerprint) return;

    File f = LittleFS.open(CERT_FILE, "w");
    if (!f) return;
    f.print(pem);
    f.close();

    File ff = LittleFS.open(CERT_FP_FILE, "w");
    if (ff) { ff.print(fp); ff.close(); }

    certCommonName   = String();
    certOrganization = String();
    certExpiration   = String();

    certPem         = pem;
    certFingerprint = fp;

    parseCertMetadata(certPem);

    mqttClient.publish(buildTopic(MQTT_CERT_STATE_TOPIC).c_str(), certFingerprint.c_str(), true);
}
