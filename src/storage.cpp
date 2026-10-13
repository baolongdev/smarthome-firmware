#include "storage.h"

#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdarg.h>
#include <time.h>

#include "hmi.h"

namespace storage {
namespace {

constexpr int kCs = 42, kMiso = 41, kSck = 48, kMosi = 47;
constexpr uint32_t kEventLogMax = 256 * 1024;
constexpr uint32_t kStatsEveryMs = 10 * 60 * 1000;  // FAT free-space scan is slow
constexpr const char* kEventLog = "/logs/events.log";
constexpr const char* kFirmware = "/firmware.bin";

enum ItemType : uint8_t { EVENT, SENSORS, FW_UPDATE, SCREENSHOT };

struct Item {
    ItemType type;
    float values[3];
    char text[176];  // EVENT: "<timestamp> <message>"
};

QueueHandle_t s_queue = nullptr;
bool s_mounted = false;
bool s_configLoaded = false;
bool s_statsDirty = true;
uint32_t s_lastStats = 0;
uint32_t s_lastCheck = 0;

bool mount() {
    pinMode(kCs, OUTPUT);
    digitalWrite(kCs, HIGH);
    SPI.begin(kSck, kMiso, kMosi, kCs);
    // Fast first; some cards only come up at the vendor's 1 MHz.
    for (uint32_t hz : {20000000u, 4000000u, 1000000u}) {
        if (SD.begin(kCs, SPI, hz) && SD.cardType() != CARD_NONE) {
            SD.mkdir("/logs");
            SD.mkdir("/logs/sensors");
            Serial.printf("SD: mounted at %lu Hz, %lu MB\n", (unsigned long)hz,
                          (unsigned long)(SD.cardSize() / (1024 * 1024)));
            return true;
        }
        SD.end();
    }
    return false;
}

// "2026-10-13 14:05:09" when NTP is synced, else "+123s" uptime.
void timestamp(char* buf, size_t len) {
    struct tm t;
    if (getLocalTime(&t, 0)) strftime(buf, len, "%Y-%m-%d %H:%M:%S", &t);
    else snprintf(buf, len, "+%lus", (unsigned long)(millis() / 1000));
}

void enqueue(const Item& it) {
    if (s_queue) xQueueSend(s_queue, &it, 0);
}

// ----- work done inside the storage task -----

void writeEvent(const char* line) {
    File f = SD.open(kEventLog, FILE_READ);
    const size_t size = f ? f.size() : 0;
    if (f) f.close();
    if (size >= kEventLogMax) {
        SD.remove("/logs/events.1.log");
        SD.rename(kEventLog, "/logs/events.1.log");
    }
    f = SD.open(kEventLog, FILE_APPEND);
    if (!f) return;
    f.println(line);
    f.close();
}

void writeSensors(const float* v) {
    char path[40];
    struct tm t;
    if (getLocalTime(&t, 0)) strftime(path, sizeof(path), "/logs/sensors/%Y-%m-%d.csv", &t);
    else strlcpy(path, "/logs/sensors/undated.csv", sizeof(path));
    const bool fresh = !SD.exists(path);
    File f = SD.open(path, FILE_APPEND);
    if (!f) return;
    if (fresh) f.println("time,primary,metric1,metric2");
    char ts[24];
    timestamp(ts, sizeof(ts));
    f.printf("%s,%.2f,%.1f,%.1f\n", ts, v[0], v[1], v[2]);
    f.close();
}

void installFirmware() {
    File f = SD.open(kFirmware, FILE_READ);
    if (!f) return;
    const size_t size = f.size();
    if (!Update.begin(size, U_FLASH)) {
        logEvent("SD update: begin failed (%s)", Update.errorString());
        f.close();
        return;
    }
    logEvent("SD firmware update started (%u bytes)", (unsigned)size);
    hmi::setUpdateProgress(0);
    static uint8_t buf[4096];
    size_t done = 0;
    while (done < size) {
        const size_t n = f.read(buf, sizeof(buf));
        if (n == 0 || Update.write(buf, n) != n) break;
        done += n;
        hmi::setUpdateProgress(done * 100 / size);  // UI task renders it
    }
    f.close();
    if (done != size || !Update.end(true)) {
        hmi::setUpdateProgress(-1);
        Serial.printf("SD update failed at %u/%u (%s)\n", (unsigned)done, (unsigned)size, Update.errorString());
        char line[120];
        snprintf(line, sizeof(line), "SD update failed at %u/%u bytes", (unsigned)done, (unsigned)size);
        writeEvent(line);
        Update.abort();
        return;
    }
    SD.remove("/firmware.installed.bin");
    SD.rename(kFirmware, "/firmware.installed.bin");
    writeEvent("SD update OK, restarting");
    delay(300);
    ESP.restart();
}

void saveScreenshot() {
    SD.mkdir("/screenshots");
    char path[40];
    for (int i = 1; i < 1000; i++) {
        snprintf(path, sizeof(path), "/screenshots/shot_%03d.bmp", i);
        if (SD.exists(path)) continue;
        File f = SD.open(path, FILE_WRITE);
        const bool ok = f && hmi::screenshotBmp(f);
        if (f) f.close();
        Serial.printf(ok ? "Saved %s\n" : "Screenshot failed: %s\n", path);
        s_statsDirty = true;
        return;
    }
}

void refreshStats(uint32_t now) {
    if (!s_statsDirty && now - s_lastStats < kStatsEveryMs) return;
    s_statsDirty = false;
    s_lastStats = now;
    hmi::StorageInfo info;
    info.present = s_mounted;
    info.configLoaded = s_configLoaded;
    if (s_mounted) {
        const uint32_t t0 = millis();
        info.totalMB = SD.totalBytes() / (1024 * 1024);
        info.usedMB = SD.usedBytes() / (1024 * 1024);  // walks the FAT: can take seconds
        const uint32_t took = millis() - t0;
        if (took > 200) Serial.printf("SD: free-space scan took %lu ms\n", (unsigned long)took);
        if (SD.exists(kFirmware)) {
            File f = SD.open(kFirmware, FILE_READ);
            info.firmwareKB = f ? f.size() / 1024 : 0;
            if (f) f.close();
        }
    }
    hmi::setStorage(info);
}

void checkCard(uint32_t now) {
    if (now - s_lastCheck < 5000) return;
    s_lastCheck = now;
    if (s_mounted) {
        File root = SD.open("/");  // fails once the card is pulled
        if (root) {
            root.close();
            return;
        }
        SD.end();
        s_mounted = false;
        s_statsDirty = true;
        Serial.println("SD: card removed");
    } else if (mount()) {
        s_mounted = true;
        s_statsDirty = true;
        writeEvent("SD card inserted");
    }
}

void storageTask(void*) {
    Item it;
    for (;;) {
        if (xQueueReceive(s_queue, &it, pdMS_TO_TICKS(1000)) == pdTRUE && s_mounted) {
            switch (it.type) {
                case EVENT: writeEvent(it.text); break;
                case SENSORS: writeSensors(it.values); break;
                case FW_UPDATE: installFirmware(); break;
                case SCREENSHOT: saveScreenshot(); break;
            }
        }
        const uint32_t now = millis();
        checkCard(now);
        refreshStats(now);
    }
}

}  // namespace

bool begin() {
    s_mounted = mount();
    if (!s_mounted) Serial.println("SD: no card");
    s_lastCheck = millis();
    return s_mounted;
}

bool loadConfig(Config& out) {
    out = Config{};
    if (!s_mounted || !SD.exists("/config.json")) return false;  // exists(): avoids a VFS error log
    File f = SD.open("/config.json", FILE_READ);
    if (!f) return false;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        logEvent("config.json invalid: %s", err.c_str());
        return false;
    }
    for (JsonObject w : doc["wifi"].as<JsonArray>()) {
        if (out.wifiCount >= kMaxWifi) break;
        const char* ssid = w["ssid"] | "";
        if (!ssid[0]) continue;
        strlcpy(out.wifiSsid[out.wifiCount], ssid, sizeof(out.wifiSsid[0]));
        strlcpy(out.wifiPass[out.wifiCount], w["password"] | "", sizeof(out.wifiPass[0]));
        out.wifiCount++;
    }
    strlcpy(out.mqttHost, doc["mqtt"]["host"] | "", sizeof(out.mqttHost));
    out.mqttPort = doc["mqtt"]["port"] | 0;
    strlcpy(out.mqttUser, doc["mqtt"]["user"] | "", sizeof(out.mqttUser));
    strlcpy(out.mqttPass, doc["mqtt"]["password"] | "", sizeof(out.mqttPass));
    strlcpy(out.room, doc["room"] | "", sizeof(out.room));
    for (JsonObject t : doc["tasmota"].as<JsonArray>()) {
        if (out.tasmotaCount >= hmi::kMaxDevices) break;
        const char* host = t["host"] | "";
        if (!host[0]) continue;
        hmi::DeviceInfo& d = out.tasmota[out.tasmotaCount++];
        strlcpy(d.name, t["name"] | host, sizeof(d.name));
        strlcpy(d.host, host, sizeof(d.host));
        d.channels = constrain(t["channels"] | 1, 1, (int)hmi::kMaxChannels);
        uint8_t c = 0;
        for (const char* l : t["labels"].as<JsonArray>()) {
            if (c >= hmi::kMaxChannels) break;
            strlcpy(d.labels[c++], l ? l : "", sizeof(d.labels[0]));
        }
    }
    out.loaded = s_configLoaded = true;
    logEvent("config.json loaded: %u Wi-Fi, mqtt=%s", out.wifiCount, out.mqttHost[0] ? out.mqttHost : "-");
    return true;
}

void start() {
    if (s_queue) return;
    s_queue = xQueueCreate(24, sizeof(Item));
    xTaskCreatePinnedToCore(storageTask, "storage", 6 * 1024, nullptr, 1, nullptr, 0);
}

void logEvent(const char* fmt, ...) {
    Item it{EVENT, {}, {}};
    char ts[24];
    timestamp(ts, sizeof(ts));
    const int n = snprintf(it.text, sizeof(it.text), "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it.text + n, sizeof(it.text) - n, fmt, ap);
    va_end(ap);
    Serial.printf("[event] %s\n", it.text + n);
    if (s_queue) enqueue(it);
    else if (s_mounted) writeEvent(it.text);  // during setup, before the task exists
}

void logSensors(float primary, float metric1, float metric2) {
    Item it{SENSORS, {primary, metric1, metric2}, {}};
    enqueue(it);
}

void requestFirmwareUpdate() {
    Item it{FW_UPDATE, {}, {}};
    enqueue(it);
}

void requestScreenshot() {
    Item it{SCREENSHOT, {}, {}};
    enqueue(it);
}

}  // namespace storage
