#include "tasmota.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "config.h"
#include "storage.h"

namespace tasmota {
namespace {

constexpr uint16_t kHttpTimeoutMs = 1500;
constexpr uint32_t kOfflineRetryMs = 15000;  // back off from devices that do not answer

struct Toggle {
    uint8_t device;
    uint8_t channel;
};

struct Device {
    hmi::DeviceInfo info;
    uint8_t mask = 0;
    bool online = false;
    bool known = false;
    uint32_t nextPoll = 0;
};

Device s_dev[hmi::kMaxDevices];
uint8_t s_count = 0;
QueueHandle_t s_queue = nullptr;

// GET /cm?cmnd=<cmnd>; fills `doc` from the JSON reply.  False on timeout/error.
bool command(const Device& d, const char* cmnd, JsonDocument& doc) {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);
    char url[96];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=%s", d.info.host, cmnd);
    if (!http.begin(client, url)) return false;
    const int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) ok = !deserializeJson(doc, http.getStream());
    http.end();
    return ok;
}

// Applies every "POWERn" (or bare "POWER" on single-relay devices) in `doc`.
uint8_t applyPower(uint8_t mask, JsonDocument& doc, uint8_t channels) {
    for (uint8_t c = 0; c < channels; c++) {
        char key[8];
        snprintf(key, sizeof(key), "POWER%u", c + 1);
        JsonVariant v = doc[key];
        if (v.isNull() && c == 0) v = doc["POWER"];
        if (v.isNull()) continue;
        if (strcmp(v.as<const char*>(), "ON") == 0) mask |= 1u << c;
        else mask &= ~(1u << c);
    }
    return mask;
}

void setOnline(uint8_t i, bool online) {
    Device& d = s_dev[i];
    if (d.known && d.online != online) {
        storage::logEvent("Tasmota %s (%s) %s", d.info.name, d.info.host, online ? "online" : "offline");
    }
    d.online = online;
    d.known = true;
    hmi::setDeviceState(i, d.mask, online);
}

void poll(uint8_t i, uint32_t now) {
    Device& d = s_dev[i];
    JsonDocument doc;
    const bool ok = command(d, "Power0", doc);
    if (ok) d.mask = applyPower(d.mask, doc, d.info.channels);
    setOnline(i, ok);
    d.nextPoll = now + (ok ? TASMOTA_POLL_MS : kOfflineRetryMs);
}

void doToggle(const Toggle& t) {
    if (t.device >= s_count) return;
    Device& d = s_dev[t.device];
    if (t.channel >= d.info.channels) return;
    char cmnd[24];
    snprintf(cmnd, sizeof(cmnd), "Power%u%%20TOGGLE", t.channel + 1);
    JsonDocument doc;
    if (command(d, cmnd, doc)) {
        d.mask = applyPower(d.mask, doc, d.info.channels);
        setOnline(t.device, true);
        storage::logEvent("Tasmota %s ch%u %s", d.info.name, t.channel + 1,
                          (d.mask >> t.channel) & 1 ? "ON" : "OFF");
    } else {
        setOnline(t.device, false);
    }
    d.nextPoll = millis() + TASMOTA_POLL_MS;
}

void devicesTask(void*) {
    for (;;) {
        Toggle t;
        // Toggles first: they are what the user is waiting for.
        if (xQueueReceive(s_queue, &t, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (WiFi.status() == WL_CONNECTED) doToggle(t);
            continue;
        }
        if (WiFi.status() != WL_CONNECTED) continue;
        const uint32_t now = millis();
        for (uint8_t i = 0; i < s_count; i++) {
            if ((int32_t)(now - s_dev[i].nextPoll) >= 0) {
                poll(i, now);
                break;  // one HTTP request per pass keeps toggles responsive
            }
        }
    }
}

}  // namespace

void start(const hmi::DeviceInfo* list, uint8_t count) {
    s_count = count > hmi::kMaxDevices ? hmi::kMaxDevices : count;
    for (uint8_t i = 0; i < s_count; i++) s_dev[i].info = list[i];
    s_queue = xQueueCreate(8, sizeof(Toggle));
    xTaskCreatePinnedToCore(devicesTask, "devices", 6 * 1024, nullptr, 2, nullptr, 0);
}

void print(Print& out) {
    for (uint8_t i = 0; i < s_count; i++) {
        const Device& d = s_dev[i];
        out.printf("  %-16s %-15s %s mask=0x%02X\n", d.info.name, d.info.host,
                   !d.known ? "pending" : (d.online ? "online " : "OFFLINE"), d.mask);
    }
}

void toggle(uint8_t device, uint8_t channel) {
    Toggle t{device, channel};
    if (s_queue) xQueueSend(s_queue, &t, 0);
}

}  // namespace tasmota
