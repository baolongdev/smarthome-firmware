#include "tasmota.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "config.h"
#include "storage.h"

namespace tasmota {
namespace {

constexpr uint8_t kFailsForOffline = 3;  // consecutive failed polls before "Ngoại tuyến"

struct Toggle {
    uint8_t device;
    uint8_t channel;
};

struct Device {
    hmi::DeviceInfo info;   // info.key: device name on the gateway ("primary", ...)
    uint8_t mask = 0;
    bool online = false;
    bool known = false;
    uint8_t fails = 0;      // consecutive failures
    uint32_t okCount = 0, failCount = 0;
    int lastError = 0;      // HTTPClient code of the last failure
    uint32_t lastMs = 0;    // duration of the last successful request
};

Device s_dev[hmi::kMaxDevices];
uint8_t s_count = 0;
QueueHandle_t s_queue = nullptr;
char s_api[128] = "";
uint32_t s_nextPoll = 0;

// One TLS connection to the gateway, kept open (HTTP/1.1 keep-alive): ~0.4 s
// per request instead of a ~3 s TLS handshake.  A small HTTP client of our
// own, because Arduino's HTTPClient does not consume the end of a chunked
// body (Cloudflare answers chunked): the next request on the reused socket
// then reads those bytes and stalls until its timeout.
WiFiClientSecure s_tls;
char s_host[64] = "";
char s_path[96] = "/";

void parseUrl() {
    const char* p = strstr(s_api, "://");
    p = p ? p + 3 : s_api;
    const char* slash = strchr(p, '/');
    const size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    strlcpy(s_host, p, std::min(hl + 1, sizeof(s_host)));
    strlcpy(s_path, slash ? slash : "/", sizeof(s_path));
}

// One line (without CRLF) before `deadline`; false on timeout or closed socket.
bool readLine(char* buf, size_t len, uint32_t deadline) {
    size_t n = 0;
    while ((int32_t)(deadline - millis()) > 0) {
        if (!s_tls.available()) {
            if (!s_tls.connected()) return false;
            vTaskDelay(1);
            continue;
        }
        const int c = s_tls.read();
        if (c < 0) continue;
        if (c == '\n') {
            if (n && buf[n - 1] == '\r') n--;
            buf[n] = 0;
            return true;
        }
        if (n + 1 < len) buf[n++] = (char)c;
    }
    return false;
}

bool readBytes(char* buf, size_t want, uint32_t deadline) {
    size_t n = 0;
    while (n < want && (int32_t)(deadline - millis()) > 0) {
        const int got = s_tls.available() ? s_tls.read(reinterpret_cast<uint8_t*>(buf + n), want - n) : 0;
        if (got > 0) n += got;
        else if (!s_tls.connected()) return false;
        else vTaskDelay(1);
    }
    return n == want;
}

// GET (body == nullptr) or POST JSON; the whole response is read so the
// socket is clean for the next request.  `firstByteMs`: how long to wait for
// the status line.
bool requestOnce(const char* body, JsonDocument& doc, int* codeOut, uint32_t firstByteMs) {
    *codeOut = -1;
    if (!s_tls.connected()) {
        s_tls.stop();
        s_tls.setInsecure();  // no CA pinning: the gateway only toggles relays
        if (!s_tls.connect(s_host, 443, 6000)) return false;
    }
    char req[320];
    const int n = body ? snprintf(req, sizeof(req),
                                  "POST %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\nAccept: application/json\r\n"
                                  "Content-Type: application/json\r\nContent-Length: %u\r\n\r\n%s",
                                  s_path, s_host, (unsigned)strlen(body), body)
                       : snprintf(req, sizeof(req),
                                  "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\nAccept: application/json\r\n\r\n",
                                  s_path, s_host);
    if (s_tls.write(reinterpret_cast<const uint8_t*>(req), n) != (size_t)n) return false;

    char line[160];
    if (!readLine(line, sizeof(line), millis() + firstByteMs)) return false;
    int code = 0;
    if (sscanf(line, "HTTP/%*s %d", &code) != 1) return false;
    *codeOut = code;
    const uint32_t deadline = millis() + 6000;  // rest of the response
    long length = -1;
    bool chunked = false, closeAfter = false;
    while (true) {  // headers
        if (!readLine(line, sizeof(line), deadline)) return false;
        if (!line[0]) break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) length = atol(line + 15);
        else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strcasestr(line, "chunked")) chunked = true;
        else if (strncasecmp(line, "Connection:", 11) == 0 && strcasestr(line, "close")) closeAfter = true;
    }
    static char bodyBuf[2048];
    size_t used = 0;
    bool ok = true;
    if (chunked) {
        for (;;) {
            if (!readLine(line, sizeof(line), deadline)) return false;
            const size_t size = strtoul(line, nullptr, 16);
            if (size == 0) {
                readLine(line, sizeof(line), deadline);  // CRLF after the last chunk
                break;
            }
            if (used + size >= sizeof(bodyBuf)) return false;
            if (!readBytes(bodyBuf + used, size, deadline)) return false;
            used += size;
            readLine(line, sizeof(line), deadline);  // CRLF after the chunk
        }
    } else if (length >= 0) {
        if ((size_t)length >= sizeof(bodyBuf) || !readBytes(bodyBuf, length, deadline)) return false;
        used = length;
    } else {
        closeAfter = true;  // body runs to the end of the connection: not supported, drop it
        ok = false;
    }
    bodyBuf[used] = 0;
    if (closeAfter) s_tls.stop();
    return ok && code == 200 && !deserializeJson(doc, bodyBuf, used);
}

bool request(const char* body, JsonDocument& doc, int* codeOut) {
    const bool reused = s_tls.connected();
    // A reused socket answers in ~0.4 s or is dead: short wait, then a fresh one.
    if (requestOnce(body, doc, codeOut, reused ? 2000 : 6000)) return true;
    s_tls.stop();
    if (requestOnce(body, doc, codeOut, 6000)) return true;
    s_tls.stop();
    return false;
}

// {"power1":true,...} -> relay mask; false for null (gateway could not reach it)
bool applyStates(Device& d, JsonVariant st) {
    if (!st.is<JsonObject>()) return false;
    uint8_t mask = 0;
    for (uint8_t c = 0; c < d.info.channels; c++) {
        char key[8];
        snprintf(key, sizeof(key), "power%u", c + 1);
        if (st[key] | false) mask |= 1u << c;
    }
    d.mask = mask;
    return true;
}

void setOnline(uint8_t i, bool online) {
    Device& d = s_dev[i];
    if (d.known && d.online != online) {
        storage::logEvent("Tasmota %s %s", d.info.name, online ? "online" : "offline");
    }
    d.online = online;
    d.known = true;
    hmi::setDeviceState(i, d.mask, online);
}

// GET <api> -> {"states":{"primary":{...},"secondary":null}}: all devices at once.
void poll(uint32_t now) {
    JsonDocument doc;
    int code = 0;
    const uint32_t t0 = millis();
    const bool ok = request(nullptr, doc, &code);
    const uint32_t took = millis() - t0;
    for (uint8_t i = 0; i < s_count; i++) {
        Device& d = s_dev[i];
        if (ok && applyStates(d, doc["states"][d.info.key])) {
            d.okCount++;
            d.lastMs = took;
            d.fails = 0;
            setOnline(i, true);
        } else {
            d.failCount++;
            d.lastError = code;
            // keep showing the last state through short dropouts
            if (++d.fails >= kFailsForOffline || !d.known) setOnline(i, false);
        }
    }
    s_nextPoll = now + TASMOTA_POLL_MS;
}

// POST <api> {"device":"primary","channel":1,"action":"toggle"} -> {"states":{...}}
void doToggle(const Toggle& t) {
    if (t.device >= s_count) return;
    Device& d = s_dev[t.device];
    if (t.channel >= d.info.channels) return;
    // Optimistic: flip the switch now, correct it from the answer.
    const uint8_t before = d.mask;
    d.mask ^= 1u << t.channel;
    hmi::setDeviceState(t.device, d.mask, d.online);
    char body[96];
    snprintf(body, sizeof(body), "{\"device\":\"%s\",\"channel\":%u,\"action\":\"toggle\"}", d.info.key, t.channel + 1);
    JsonDocument doc;
    int code = 0;
    if (request(body, doc, &code) && applyStates(d, doc["states"])) {
        d.fails = 0;
        setOnline(t.device, true);
        storage::logEvent("Tasmota %s ch%u %s", d.info.name, t.channel + 1, (d.mask >> t.channel) & 1 ? "ON" : "OFF");
    } else {
        d.mask = before;
        d.lastError = code;
        hmi::setDeviceState(t.device, d.mask, d.online);
        storage::logEvent("Tasmota %s ch%u: gateway error (HTTP %d)", d.info.name, t.channel + 1, code);
    }
    s_nextPoll = millis() + TASMOTA_POLL_MS;
}

void devicesTask(void*) {
    for (;;) {
        Toggle t;
        // Toggles first: they are what the user is waiting for.
        if (xQueueReceive(s_queue, &t, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (WiFi.status() == WL_CONNECTED) doToggle(t);
            continue;
        }
        if (WiFi.status() != WL_CONNECTED || !s_count) continue;
        const uint32_t now = millis();
        if ((int32_t)(now - s_nextPoll) >= 0) poll(now);
    }
}

}  // namespace

void start(const hmi::DeviceInfo* list, uint8_t count, const char* apiUrl) {
    s_count = count > hmi::kMaxDevices ? hmi::kMaxDevices : count;
    for (uint8_t i = 0; i < s_count; i++) s_dev[i].info = list[i];
    strlcpy(s_api, apiUrl ? apiUrl : "", sizeof(s_api));
    parseUrl();
    if (!s_api[0]) s_count = 0;  // no gateway: nothing to talk to
    Serial.printf("Tasmota: %u device(s) via %s\n", s_count, s_api[0] ? s_api : "(no gateway URL)");
    s_queue = xQueueCreate(8, sizeof(Toggle));
    // 10 KB: the TLS handshake needs more stack than plain HTTP.
    xTaskCreatePinnedToCore(devicesTask, "devices", 10 * 1024, nullptr, 2, nullptr, 0);
}

void print(Print& out) {
    out.printf("  gateway: %s\n", s_api[0] ? s_api : "(none)");
    if (WiFi.status() == WL_CONNECTED)
        out.printf("  Wi-Fi %s  IP %s  RSSI %d dBm\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else
        out.printf("  Wi-Fi not connected (status %d)\n", WiFi.status());
    for (uint8_t i = 0; i < s_count; i++) {
        const Device& d = s_dev[i];
        out.printf("  %-16s %-10s %s mask=0x%02X  ok %lu fail %lu  last err %d  last %lu ms\n", d.info.name,
                   d.info.key, !d.known ? "pending" : (d.online ? "online " : "OFFLINE"), d.mask,
                   (unsigned long)d.okCount, (unsigned long)d.failCount, d.lastError, (unsigned long)d.lastMs);
    }
}

void toggle(uint8_t device, uint8_t channel) {
    Toggle t{device, channel};
    if (s_queue) xQueueSend(s_queue, &t, 0);
}

}  // namespace tasmota
