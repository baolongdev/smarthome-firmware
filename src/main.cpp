// Business logic as FreeRTOS tasks.  The LVGL HMI (src/hmi/) runs in its own
// "ui" task and is driven only through include/hmi.h.
//
//   task      core prio  owns
//   ui          1   3    LVGL, touch, backlight              (src/hmi/hmi_ui.cpp)
//   app         1   4    relays, sensors, settings (NVS)     queue: AppMsg
//   net         0   2    Wi-Fi manager, MQTT, OTA, NTP       queue: NetMsg
//   devices     0   2    Tasmota relays over HTTP            (src/tasmota.cpp)
//   storage     0   1    SD card: logs, stats, fw update     (src/storage.cpp)
//   console     0   1    serial commands
//
// Tasks never call each other's code directly: they post messages.  Shared
// state is either in the HMI model (locked) or std::atomic.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <time.h>

#include <atomic>

#include "config.h"
#include "hmi.h"
#include "lgfx_4848s040_reference.h"
#include "datalog.h"
#include "storage.h"
#include "tasmota.h"
#include "wifi_manager.h"

// Touch is read through LGFX (Touch_GT911 on I2C_NUM_1).  Do not also start
// Wire on GPIO19/45 - two drivers on the same pins corrupt touch reads.
LGFX display;

namespace {

// ===================== shared state =====================

const uint8_t kRelayPins[RELAY_COUNT] = {RELAY_PIN_1, RELAY_PIN_2, RELAY_PIN_3};
std::atomic<bool> g_relay[RELAY_COUNT];       // written by app, read by net (MQTT state)
std::atomic<uint32_t> g_relayChange[RELAY_COUNT];
std::atomic<bool> g_publishPending{false};    // app -> net: publish MQTT state
std::atomic<uint32_t> g_minFreeHeap{UINT32_MAX};
uint32_t g_bootTime = 0;

storage::Config g_sdConfig;  // /config.json, read once in setup()
char g_mqttHost[64] = MQTT_BROKER;
uint16_t g_mqttPort = MQTT_PORT;
char g_mqttUser[32] = MQTT_USER;
char g_mqttPass[64] = MQTT_PASSWORD;

// ===================== messages =====================

enum class AppCmd : uint8_t { RelayToggle, RelaySet, Sensors, SensorPreview, SaveBrightness, SaveAutoDim, SaveTheme };
struct AppMsg {
    AppCmd cmd;
    uint8_t index;
    bool flag;
    float v[3];
    char text[12];
};

enum class NetCmd : uint8_t { Scan, Join };
struct NetMsg {
    NetCmd cmd;
    char ssid[33];
    char pass[65];
};

QueueHandle_t g_appQ = nullptr;
QueueHandle_t g_netQ = nullptr;

void postApp(const AppMsg& m) { xQueueSend(g_appQ, &m, 0); }
void postNet(const NetMsg& m) { xQueueSend(g_netQ, &m, 0); }

// ===================== app task =====================

Preferences g_prefs;

struct SensorValues {
    float primary = 0, metric1 = 0, metric2 = 0;
    bool primaryError = false;
    bool stale = false;
};

void setRelay(int index, bool on) {
    if (index < 0 || index >= RELAY_COUNT || g_relay[index] == on) return;
    g_relay[index] = on;
    g_relayChange[index] = millis();
    digitalWrite(kRelayPins[index], on ? HIGH : LOW);
    hmi::setRelay(index, on);
    storage::logEvent("Relay %d %s", index + 1, on ? "ON" : "OFF");
    g_publishPending = true;
}

void pushSensors(const SensorValues& s, hmi::SensorState state) {
    hmi::setPrimary(s.primary, s.primaryError ? hmi::SensorState::Error : state);
    hmi::setMetric(0, s.metric1, state);
    hmi::setMetric(1, s.metric2, state);
    static uint32_t lastLog = 0;
    if (state == hmi::SensorState::Ok && !s.primaryError && (!lastLog || millis() - lastLog >= 60000)) {
        lastLog = millis();
        storage::logSensors(s.primary, s.metric1, s.metric2);
    }
    if (state == hmi::SensorState::Ok && !s.primaryError) datalog::addSample(s.primary, s.metric1, s.metric2);
}

void appTask(void*) {
    SensorValues sensors;
    bool simulate = HMI_SIMULATE_SENSORS;
    uint32_t lastSim = 0, lastReal = 0;
    esp_task_wdt_add(nullptr);

    for (;;) {
        AppMsg m;
        datalog::tick();  // close finished history slots (10 s / 1 min)
        if (xQueueReceive(g_appQ, &m, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (m.cmd) {
                case AppCmd::RelayToggle:
                    if (m.index < RELAY_COUNT) setRelay(m.index, !g_relay[m.index]);
                    break;
                case AppCmd::RelaySet: setRelay(m.index, m.flag); break;
                case AppCmd::Sensors:  // real data (MQTT): simulation off for good
                    simulate = false;
                    lastReal = millis();
                    sensors.stale = false;
                    sensors.primaryError = m.flag;
                    if (!isnan(m.v[0])) sensors.primary = m.v[0];
                    if (!isnan(m.v[1])) sensors.metric1 = m.v[1];
                    if (!isnan(m.v[2])) sensors.metric2 = m.v[2];
                    pushSensors(sensors, hmi::SensorState::Ok);
                    break;
                case AppCmd::SensorPreview: {  // console "sensor ok|warn|error|loading|stale|sim"
                    const char* s = m.text;
                    simulate = strcmp(s, "sim") == 0;
                    if (strcmp(s, "ok") == 0) hmi::setPrimary(26.4f);
                    else if (strcmp(s, "warn") == 0) hmi::setPrimary(31.8f);
                    else if (strcmp(s, "error") == 0) hmi::setPrimary(0, hmi::SensorState::Error);
                    else if (strcmp(s, "loading") == 0) hmi::setPrimary(0, hmi::SensorState::Loading);
                    else if (strcmp(s, "stale") == 0) hmi::setPrimary(26.4f, hmi::SensorState::Stale);
                    break;
                }
                case AppCmd::SaveBrightness: g_prefs.putUChar("bright", m.index); break;
                case AppCmd::SaveAutoDim: g_prefs.putBool("autodim", m.flag); break;
                case AppCmd::SaveTheme: g_prefs.putUChar("theme", m.index); break;
            }
        }

        const uint32_t now = millis();
        if (simulate && now - g_bootTime > 1500 && (!lastSim || now - lastSim >= 2000)) {
            // No sensor wired yet: random walk until real data arrives on MQTT_TOPIC_SENSOR.
            if (!lastSim) sensors = SensorValues{26.4f, 62, 412};
            lastSim = now;
            sensors.primary = constrain(sensors.primary + random(-60, 61) / 100.0f, 23.5f, 28.5f);
            sensors.metric1 = constrain(sensors.metric1 + random(-2, 3), 55.0f, 70.0f);
            sensors.metric2 = constrain(sensors.metric2 + random(-30, 31), 320.0f, 520.0f);
            pushSensors(sensors, hmi::SensorState::Ok);
        }
        if (!simulate && lastReal && !sensors.stale && now - lastReal > HMI_SENSOR_STALE_MS) {
            sensors.stale = true;
            storage::logEvent("Sensor data stale");
            pushSensors(sensors, hmi::SensorState::Stale);
        }
        esp_task_wdt_reset();
    }
}

// ===================== net task =====================

WiFiClient g_wifiClient;
PubSubClient g_mqtt(g_wifiClient);

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("MQTT [%s]: %.*s\n", topic, length, (const char*)payload);
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) return;

    AppMsg m{};
    if (strcmp(topic, MQTT_TOPIC_SENSOR) == 0) {
        // {"primary":26.4,"metric1":62,"metric2":412}  or  {"error":true}
        m.cmd = AppCmd::Sensors;
        m.flag = doc["error"] | false;
        m.v[0] = doc["primary"].is<float>() ? doc["primary"].as<float>() : NAN;
        m.v[1] = doc["metric1"].is<float>() ? doc["metric1"].as<float>() : NAN;
        m.v[2] = doc["metric2"].is<float>() ? doc["metric2"].as<float>() : NAN;
        postApp(m);
        return;
    }
    if (strcmp(topic, MQTT_TOPIC_COMMAND) != 0) return;
    if (doc["relay"].is<int>()) {
        m.cmd = AppCmd::RelaySet;
        m.index = doc["relay"].as<int>();
        m.flag = doc["state"] | false;
        postApp(m);
    } else if (doc["relays"].is<JsonArray>()) {
        JsonArray relays = doc["relays"];
        for (size_t i = 0; i < relays.size() && i < RELAY_COUNT; i++) {
            postApp(AppMsg{AppCmd::RelaySet, static_cast<uint8_t>(i), relays[i].as<bool>(), {}, {}});
        }
    } else if (doc["brightness"].is<int>()) {
        // 0-255 for backwards compatibility; the HMI works in percent.
        hmi::setBrightness(map(doc["brightness"].as<int>(), 0, 255, 0, 100));
    }
}

void publishState() {
    if (!g_mqtt.connected()) return;
    JsonDocument doc;
    doc["device"] = DEVICE_NAME;
    doc["type"] = DEVICE_TYPE;
    doc["fw_version"] = FIRMWARE_VERSION;
    doc["hw_version"] = HARDWARE_VERSION;
    doc["uptime"] = (millis() - g_bootTime) / 1000;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["min_free_heap"] = g_minFreeHeap.load();
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();
    JsonArray relays = doc["relays"].to<JsonArray>();
    for (int i = 0; i < RELAY_COUNT; i++) {
        JsonObject r = relays.add<JsonObject>();
        r["index"] = i;
        r["state"] = g_relay[i].load();
        r["last_change"] = (millis() - g_relayChange[i]) / 1000;
    }
    String out;
    serializeJson(doc, out);
    g_mqtt.publish(MQTT_TOPIC_STATE, out.c_str(), true);
}

void setupOTA() {
    ArduinoOTA.setHostname(WIFI_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.onStart([]() { storage::logEvent("OTA start"); });
    ArduinoOTA.onError([](ota_error_t e) { storage::logEvent("OTA error %u", e); });
    ArduinoOTA.begin();
}

// MQTT with exponential backoff (5..60 s) and a 1 s TCP timeout.
void maintainMqtt(uint32_t now) {
    static uint32_t lastTry = 0, retryDelay = MQTT_RECONNECT_INTERVAL;
    if (!wifimgr::connected() || g_mqtt.connected()) return;
    if (lastTry && now - lastTry < retryDelay) return;
    lastTry = now;
    String id = String(MQTT_CLIENT_ID) + "-" + String(random(0xffff), HEX);
    if (g_mqtt.connect(id.c_str(), g_mqttUser[0] ? g_mqttUser : nullptr, g_mqttPass[0] ? g_mqttPass : nullptr,
                       MQTT_TOPIC_AVAILABILITY, 1, true, "offline")) {
        storage::logEvent("MQTT connected to %s:%u", g_mqttHost, g_mqttPort);
        g_mqtt.subscribe(MQTT_TOPIC_COMMAND);
        g_mqtt.subscribe(MQTT_TOPIC_CONFIG);
        g_mqtt.subscribe(MQTT_TOPIC_SENSOR);
        g_mqtt.publish(MQTT_TOPIC_AVAILABILITY, "online", true);
        publishState();
        retryDelay = MQTT_RECONNECT_INTERVAL;
    } else {
        Serial.printf("MQTT connect failed rc=%d, retry in %lus\n", g_mqtt.state(), retryDelay / 1000);
        retryDelay = min<uint32_t>(retryDelay * 2, 60000);
    }
}

// Link, clock and system info for the header / Settings, once a second.
void feedHmiStatus(uint32_t now) {
    static uint32_t last = 0;
    static uint32_t lastSystemInfo = 0;
    if (last && now - last < 1000) return;
    last = now;
    const bool up = wifimgr::connected();
    hmi::Link link = hmi::Link::Offline;
    if (up) link = g_mqtt.connected() ? hmi::Link::Online : hmi::Link::BrokerDown;
    else if (now - g_bootTime < 20000) link = hmi::Link::Connecting;
    hmi::setLink(link);

    struct tm t;
    char hhmm[6] = "--:--";
    if (getLocalTime(&t, 0)) strftime(hhmm, sizeof(hhmm), "%H:%M", &t);
    hmi::setClock(hhmm);

    hmi::SystemInfo info;
    if (up) {
        strlcpy(info.ssid, WiFi.SSID().c_str(), sizeof(info.ssid));
        strlcpy(info.ip, WiFi.localIP().toString().c_str(), sizeof(info.ip));
        info.rssi = WiFi.RSSI();
        strlcpy(info.bssid, WiFi.BSSIDstr().c_str(), sizeof(info.bssid));
        info.channel = WiFi.channel();
        strlcpy(info.gateway, WiFi.gatewayIP().toString().c_str(), sizeof(info.gateway));
        strlcpy(info.subnet, WiFi.subnetMask().toString().c_str(), sizeof(info.subnet));
        strlcpy(info.dns, WiFi.dnsIP().toString().c_str(), sizeof(info.dns));
    }
    strlcpy(info.mac, WiFi.macAddress().c_str(), sizeof(info.mac));
    strlcpy(info.hostname, WIFI_HOSTNAME, sizeof(info.hostname));
    strlcpy(info.mqttHost, g_mqttHost, sizeof(info.mqttHost));
    info.mqttPort = g_mqttPort;
    info.mqttState = g_mqtt.state();
    info.mqttAuth = g_mqttUser[0] != 0;

    const uint32_t heap = ESP.getFreeHeap();
    if (heap < g_minFreeHeap) g_minFreeHeap = heap;
    info.heapKB = heap / 1024;
    info.heapMinKB = ESP.getMinFreeHeap() / 1024;
    info.heapTotalKB = ESP.getHeapSize() / 1024;
    info.heapLargestKB = ESP.getMaxAllocHeap() / 1024;
    info.psramKB = ESP.getFreePsram() / 1024;
    info.psramTotalKB = ESP.getPsramSize() / 1024;
    info.flashMB = ESP.getFlashChipSize() / (1024 * 1024);
    static const uint32_t appKB = ESP.getSketchSize() / 1024;  // hashes the image: once
    info.appKB = appKB;
    info.appSlotKB = ESP.getFreeSketchSpace() / 1024;

    info.uptimeS = (now - g_bootTime) / 1000;
    info.resetReason = static_cast<uint8_t>(esp_reset_reason());
    info.timeSynced = getLocalTime(&t, 0);
    if (info.timeSynced) strftime(info.date, sizeof(info.date), "%d/%m/%Y", &t);
    strlcpy(info.chip, ESP.getChipModel(), sizeof(info.chip));
    info.chipRev = ESP.getChipRevision();
    info.cores = ESP.getChipCores();
    info.cpuMHz = ESP.getCpuFreqMHz();
    strlcpy(info.sdk, ESP.getSdkVersion(), sizeof(info.sdk));
    // RSSI and heap can vary every second. The Settings page only needs a
    // human-readable refresh every few seconds; avoid repeated layout work.
    if (!lastSystemInfo || now - lastSystemInfo >= 5000) {
        lastSystemInfo = now;
        hmi::setSystemInfo(info);
    }
}

void netTask(void*) {
    g_wifiClient.setTimeout(1);  // bound the blocking TCP connect
    g_mqtt.setServer(g_mqttHost, g_mqttPort);
    g_mqtt.setCallback(mqttCallback);
    g_mqtt.setBufferSize(2048);
    g_mqtt.setKeepAlive(MQTT_KEEPALIVE);
    g_mqtt.setSocketTimeout(2);
    wifimgr::begin(g_sdConfig);

    bool wasUp = false, otaStarted = false;
    for (;;) {
        NetMsg m;
        if (xQueueReceive(g_netQ, &m, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (m.cmd == NetCmd::Scan) wifimgr::startScan();
            else wifimgr::join(m.ssid, m.pass);
        }
        const uint32_t now = millis();
        wifimgr::loop();

        const bool up = wifimgr::connected();
        if (up != wasUp) {
            wasUp = up;
            if (up) {
                storage::logEvent("Wi-Fi connected: %s %s RSSI %d", WiFi.SSID().c_str(),
                                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
                configTime(HMI_TZ_OFFSET_SECONDS, 0, "pool.ntp.org", "time.google.com");
                if (!otaStarted) {
                    setupOTA();
                    otaStarted = true;
                }
            } else {
                storage::logEvent("Wi-Fi disconnected");
            }
        }
        maintainMqtt(now);
        g_mqtt.loop();
        if (g_publishPending.exchange(false)) publishState();
        if (otaStarted) ArduinoOTA.handle();
        feedHmiStatus(now);
    }
}

// ===================== console task =====================

void printTasks() {
    static const char* const names[] = {"ui", "app", "net", "devices", "storage", "console"};
    Serial.printf("heap %u KB (min %u KB), psram free %u KB\n", ESP.getFreeHeap() / 1024,
                  ESP.getMinFreeHeap() / 1024, ESP.getFreePsram() / 1024);
    for (const char* n : names) {
        TaskHandle_t h = xTaskGetHandle(n);
        if (h) Serial.printf("  %-8s stack free %5u B  prio %u\n", n, uxTaskGetStackHighWaterMark(h), uxTaskPriorityGet(h));
    }
}

// "shot" (tools/screenshot.py), "shot sd", "info"/"tasks", "toggle <1..3>",
// "page <0..3>", "scroll <px>", "drag x y0 y1 [ms]", "devices", "wifi" (open + scan), "theme <0..2>",
// "sensor ok|warn|error|loading|stale|sim", "bench [psram|hybrid]" (UI build/redraw timing).
void consoleTask(void*) {
    char line[48];
    uint8_t len = 0;
    for (;;) {
        while (Serial.available()) {
            const char c = Serial.read();
            if (c != '\n' && c != '\r') {
                if (len < sizeof(line) - 1) line[len++] = c;
                continue;
            }
            line[len] = 0;
            len = 0;
            if (!line[0]) continue;
            if (strcmp(line, "shot") == 0) {
                // Core log_x() from other tasks would corrupt the binary stream.
                Serial.setDebugOutput(false);
                hmi::screenshot(Serial);
                Serial.flush();
                Serial.setDebugOutput(true);
            } else if (strncmp(line, "shot fb", 7) == 0) {  // "shot fb [ms]": real framebuffer
                Serial.setDebugOutput(false);
                hmi::screenshotFramebuffer(Serial, line[7] ? atoi(line + 8) : 0);
                Serial.flush();
                Serial.setDebugOutput(true);
            } else if (strcmp(line, "shot sd") == 0) {
                storage::requestScreenshot();
            } else if (strcmp(line, "sd tree") == 0) {
                storage::requestTree();
            } else if (strcmp(line, "sd wipe") == 0) {
                storage::requestWipe();
            } else if (strcmp(line, "sd restore") == 0) {
                storage::requestRestore();
            } else if (strcmp(line, "history save") == 0) {
                storage::requestHistorySave();
            } else if (strcmp(line, "info") == 0 || strcmp(line, "tasks") == 0) {
                printTasks();
            } else if (strncmp(line, "toggle ", 7) == 0) {
                postApp(AppMsg{AppCmd::RelayToggle, static_cast<uint8_t>(atoi(line + 7) - 1), false, {}, {}});
            } else if (strncmp(line, "page ", 5) == 0) {
                hmi::showPage(atoi(line + 5));
            } else if (strncmp(line, "scroll ", 7) == 0) {
                hmi::scrollPage(atoi(line + 7));
            } else if (strncmp(line, "drag ", 5) == 0) {
                int x = 240, y0 = 300, y1 = 120, ms = 300;  // "drag x y0 y1 [ms]"
                sscanf(line + 5, "%d %d %d %d", &x, &y0, &y1, &ms);
                hmi::injectDrag(x, y0, y1, ms);
            } else if (strncmp(line, "detail ", 7) == 0) {
                hmi::showDetail(atoi(line + 7));
            } else if (strncmp(line, "bench", 5) == 0) {
                hmi::benchmark(strstr(line, "psram") ? 1 : strstr(line, "hybrid") ? 2 : 0);
            } else if (strncmp(line, "view ", 5) == 0) {
                hmi::previewFile(line + 5);
            } else if (strcmp(line, "kbd") == 0) {
                hmi::previewKeyboard();
            } else if (strcmp(line, "devices") == 0) {
                tasmota::print(Serial);
            } else if (strcmp(line, "wifi") == 0) {
                hmi::showPage(3);
                postNet(NetMsg{NetCmd::Scan, {}, {}});
            } else if (strncmp(line, "theme ", 6) == 0) {
                hmi::setTheme(atoi(line + 6));
            } else if (strncmp(line, "sensor ", 7) == 0) {
                AppMsg m{AppCmd::SensorPreview, 0, false, {}, {}};
                strlcpy(m.text, line + 7, sizeof(m.text));
                postApp(m);
            } else {
                Serial.printf("? %s\n", line);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ===================== HMI callbacks (run in the ui task: post only) =====================

void onRelayToggle(uint8_t i) { postApp(AppMsg{AppCmd::RelayToggle, i, false, {}, {}}); }
void onBrightnessChanged(uint8_t pct) { postApp(AppMsg{AppCmd::SaveBrightness, pct, false, {}, {}}); }
void onAutoDimChanged(bool on) { postApp(AppMsg{AppCmd::SaveAutoDim, 0, on, {}, {}}); }
void onThemeChanged(uint8_t i) { postApp(AppMsg{AppCmd::SaveTheme, i, false, {}, {}}); }
void onBacklight(uint8_t pct) {
    pct = constrain(pct, 10, 100);
    display.setBrightness(map(pct, 10, 100, HMI_BACKLIGHT_MIN_DUTY, 255));
}
void onWifiScan() { postNet(NetMsg{NetCmd::Scan, {}, {}}); }
void onWifiJoin(const char* ssid, const char* pass) {
    NetMsg m{NetCmd::Join, {}, {}};
    strlcpy(m.ssid, ssid, sizeof(m.ssid));
    strlcpy(m.pass, pass, sizeof(m.pass));
    postNet(m);
}
void onFirmwareUpdate() { storage::requestFirmwareUpdate(); }
void onDeviceToggle(uint8_t device, uint8_t channel) { tasmota::toggle(device, channel); }
void onFileList(const char* path) { storage::requestList(path); }
void onFileRead(const char* path) { storage::requestRead(path); }
void onFileDelete(const char* path) { storage::requestDelete(path); }

// Tasmota devices: /config.json "tasmota" if present, else config.h.
uint8_t deviceList(hmi::DeviceInfo* out) {
    if (g_sdConfig.tasmotaCount) {
        memcpy(out, g_sdConfig.tasmota, sizeof(hmi::DeviceInfo) * g_sdConfig.tasmotaCount);
        return g_sdConfig.tasmotaCount;
    }
    const struct { const char* name; const char* host; uint8_t channels; } defaults[] = {
        {TASMOTA_1_NAME, TASMOTA_1_HOST, TASMOTA_1_CHANNELS},
        {TASMOTA_2_NAME, TASMOTA_2_HOST, TASMOTA_2_CHANNELS},
    };
    uint8_t n = 0;
    for (const auto& d : defaults) {
        out[n] = hmi::DeviceInfo{};
        strlcpy(out[n].name, d.name, sizeof(out[n].name));
        strlcpy(out[n].host, d.host, sizeof(out[n].host));
        out[n].channels = d.channels;
        n++;
    }
    return n;
}

}  // namespace

void setup() {
    Serial.begin(SERIAL_BAUDRATE);
    delay(100);
    Serial.println("\n\n=== ESP32-4848S040 Smart Home Panel ===");
    Serial.printf("Firmware: %s  Hardware: %s\n", FIRMWARE_VERSION, HARDWARE_VERSION);
    Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());  // 1=power-on/EN, 4=panic, 6/7=watchdog, 9=brownout
    g_bootTime = millis();

    for (int i = 0; i < RELAY_COUNT; i++) {
        pinMode(kRelayPins[i], OUTPUT);
        digitalWrite(kRelayPins[i], LOW);  // Active HIGH relay, OFF = LOW
        g_relay[i] = false;
        g_relayChange[i] = millis();
    }

    if (!display.begin()) Serial.println("!!! display.begin() FAILED !!!");
    // Backlight: Light_PWM attached GPIO38 to an LEDC channel in begin().  Do
    // not pinMode()/digitalWrite() it afterwards: that detaches the pin from
    // LEDC, leaves it stuck at 100 % and the brightness slider does nothing.
    display.setBrightness(255);  // until the saved level arrives via the UI

    // PSRAM history/event rings first: storage::begin() restores into them.
    datalog::begin();
    // SD after the display: SCK/MOSI are shared with the ST7701 init bus.
    storage::begin();
    storage::logEvent("Boot fw=%s reset=%d", FIRMWARE_VERSION, (int)esp_reset_reason());
    if (storage::loadConfig(g_sdConfig)) {
        if (g_sdConfig.mqttHost[0]) strlcpy(g_mqttHost, g_sdConfig.mqttHost, sizeof(g_mqttHost));
        if (g_sdConfig.mqttPort) g_mqttPort = g_sdConfig.mqttPort;
        if (g_sdConfig.mqttUser[0]) strlcpy(g_mqttUser, g_sdConfig.mqttUser, sizeof(g_mqttUser));
        if (g_sdConfig.mqttPass[0]) strlcpy(g_mqttPass, g_sdConfig.mqttPass, sizeof(g_mqttPass));
        hmi::setRoomName(g_sdConfig.room);
    }

    g_prefs.begin("hmi", false);
    hmi::setBrightness(g_prefs.getUChar("bright", 80));
    hmi::setAutoDim(g_prefs.getBool("autodim", true));
    hmi::setTheme(g_prefs.getUChar("theme", 0));

    // Make the task watchdog fatal (10 s): a hung ui/app task reboots instead of freezing.
    esp_task_wdt_init(10, true);

    g_appQ = xQueueCreate(16, sizeof(AppMsg));
    g_netQ = xQueueCreate(4, sizeof(NetMsg));

    hmi::Callbacks cb;
    cb.relayToggle = onRelayToggle;
    cb.brightnessChanged = onBrightnessChanged;
    cb.autoDimChanged = onAutoDimChanged;
    cb.themeChanged = onThemeChanged;
    cb.backlight = onBacklight;
    cb.wifiScan = onWifiScan;
    cb.wifiJoin = onWifiJoin;
    cb.firmwareUpdate = onFirmwareUpdate;
    cb.deviceToggle = onDeviceToggle;
    cb.fileList = onFileList;
    cb.fileRead = onFileRead;
    cb.fileDelete = onFileDelete;
    static hmi::DeviceInfo devices[hmi::kMaxDevices];
    const uint8_t deviceCount = deviceList(devices);
    hmi::configureDevices(devices, deviceCount);  // the Control page is built for these
    hmi::begin(display, cb);  // "ui" task, core 1
    tasmota::start(devices, deviceCount);

    storage::start();
    xTaskCreatePinnedToCore(appTask, "app", 4 * 1024, nullptr, 4, nullptr, 1);
    xTaskCreatePinnedToCore(netTask, "net", 8 * 1024, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(consoleTask, "console", 4 * 1024, nullptr, 1, nullptr, 0);
    Serial.println("Tasks started: ui app net devices storage console");
}

// Everything runs in dedicated tasks; free the Arduino loop task.
void loop() { vTaskDelete(nullptr); }
