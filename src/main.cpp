// ESP-NOW-only firmware: panel is ESP-NOW slave controlling local relays.
// Wi-Fi + optional HTTP OTA (OTA_FIRMWARE_URL in config.h).

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <time.h>
#include <atomic>

#include "config.h"
#include "hmi.h"
#include "lgfx_4848s040_reference.h"
#include "datalog.h"
#include "storage.h"
#include "wifi_manager.h"
#include "espnow_master.h"
#include "touch_gt911.h"

LGFX display;

namespace {

const uint8_t kRelayPins[RELAY_COUNT] = {RELAY_PIN_1, RELAY_PIN_2, RELAY_PIN_3};
std::atomic<bool> g_relay[RELAY_COUNT];
std::atomic<uint32_t> g_relayChange[RELAY_COUNT];
std::atomic<uint32_t> g_minFreeHeap{UINT32_MAX};
uint32_t g_bootTime = 0;
storage::Config g_sdConfig;
Preferences g_prefs;

enum class AppCmd : uint8_t { RelayToggle, RelaySet, SaveBrightness, SaveAutoDim, SaveTheme, SaveDark, SaveEspNow };
struct AppMsg { AppCmd cmd; uint8_t index; bool flag; };
struct EspCmd { uint8_t type; uint8_t index; bool on; };
enum class NetCmd : uint8_t { Scan, Join };
struct NetMsg { NetCmd cmd; char ssid[33]; char pass[65]; };
QueueHandle_t g_appQ = nullptr;
QueueHandle_t g_espQ = nullptr;
QueueHandle_t g_espnowCfgQ = nullptr;
QueueHandle_t g_netQ = nullptr;
hmi::EspNowUiConfig g_pendingEspNowUi{};
GT911Touch* g_mtGt911 = nullptr;
bool g_mtTestActive = false;
uint32_t g_mtTestEndMs = 0;

void postApp(const AppMsg& m) { if (g_appQ) xQueueSend(g_appQ, &m, 0); }

void setRelay(uint8_t i, bool on) {
    if (i >= RELAY_COUNT) return;
    digitalWrite(kRelayPins[i], on ? HIGH : LOW);
    g_relay[i] = on;
    g_relayChange[i] = millis();
    hmi::setRelay(i, on);
}

void applyEsp(const EspCmd& c) {
    if (c.type == 0) setRelay(c.index, !g_relay[c.index].load());
    else setRelay(c.index, c.on);
}

void feedStatus(uint32_t now) {
    static uint32_t last = 0, lastInfo = 0;
    static bool wasWifi = false, ntpStarted = false;
    if (last && now - last < 1000) return;
    last = now;
    const bool wifi = wifimgr::connected();
    if (wifi && !wasWifi) {
        configTime(HMI_TZ_OFFSET_SECONDS, 0, "pool.ntp.org", "time.google.com");
        ntpStarted = true;
        storage::logEvent("NTP sync requested (TZ+7)");
    }
    wasWifi = wifi;
    if (wifi) hmi::setLink(hmi::Link::Online);
    else hmi::setLink(espnow::ready() ? hmi::Link::Connecting : hmi::Link::Connecting);

    struct tm t;
    char hhmm[6] = "--:--";
    const bool synced = getLocalTime(&t, 0);
    if (synced) strftime(hhmm, sizeof(hhmm), "%H:%M", &t);
    hmi::setClock(hhmm);

    if (lastInfo && now - lastInfo < 2000) return;
    lastInfo = now;
    hmi::SystemInfo info{};
    const uint8_t ch = wifi ? WiFi.channel() : (g_sdConfig.espnow.channel ? g_sdConfig.espnow.channel : ESPNOW_CHANNEL);
    info.channel = ch;
    if (wifi) {
        strlcpy(info.ssid, WiFi.SSID().c_str(), sizeof(info.ssid));
        strlcpy(info.ip, WiFi.localIP().toString().c_str(), sizeof(info.ip));
        info.rssi = WiFi.RSSI();
        strlcpy(info.bssid, WiFi.BSSIDstr().c_str(), sizeof(info.bssid));
        strlcpy(info.gateway, WiFi.gatewayIP().toString().c_str(), sizeof(info.gateway));
        strlcpy(info.subnet, WiFi.subnetMask().toString().c_str(), sizeof(info.subnet));
        strlcpy(info.dns, WiFi.dnsIP().toString().c_str(), sizeof(info.dns));
    } else {
        snprintf(info.ssid, sizeof(info.ssid), "ESP-NOW CH%u", ch);
        strlcpy(info.ip, "--", sizeof(info.ip));
    }
    info.espnowReady = espnow::ready();
    info.espnowNodes = espnow::nodeCount();
    strlcpy(info.espnowStatus, espnow::statusText(), sizeof(info.espnowStatus));
    strlcpy(info.hostname, WIFI_HOSTNAME, sizeof(info.hostname));
    static char mac[18] = "";
    if (!mac[0]) strlcpy(mac, WiFi.macAddress().c_str(), sizeof(mac));
    strlcpy(info.mac, mac, sizeof(info.mac));
    const uint32_t heap = ESP.getFreeHeap();
    if (heap < g_minFreeHeap) g_minFreeHeap = heap;
    info.heapKB = heap / 1024;
    info.heapMinKB = ESP.getMinFreeHeap() / 1024;
    info.heapTotalKB = ESP.getHeapSize() / 1024;
    info.heapLargestKB = ESP.getMaxAllocHeap() / 1024;
    info.psramKB = ESP.getFreePsram() / 1024;
    info.psramTotalKB = ESP.getPsramSize() / 1024;
    info.flashMB = ESP.getFlashChipSize() / (1024 * 1024);
    static const uint32_t appKB = ESP.getSketchSize() / 1024;
    info.appKB = appKB;
    info.appSlotKB = ESP.getFreeSketchSpace() / 1024;
    info.uptimeS = (now - g_bootTime) / 1000;
    info.resetReason = static_cast<uint8_t>(esp_reset_reason());
    info.timeSynced = synced;
    if (synced) strftime(info.date, sizeof(info.date), "%d/%m/%Y", &t);
    strlcpy(info.chip, ESP.getChipModel(), sizeof(info.chip));
    info.chipRev = ESP.getChipRevision();
    info.cores = ESP.getChipCores();
    info.cpuMHz = ESP.getCpuFreqMHz();
    strlcpy(info.sdk, ESP.getSdkVersion(), sizeof(info.sdk));
    hmi::setSystemInfo(info);
}

hmi::EspNowUiConfig espNowUiConfig();
bool parseMacUi(const char* text, uint8_t mac[6]);
void applyEspNowUiConfig(const hmi::EspNowUiConfig& ui);

void appTask(void*) {
    esp_task_wdt_add(nullptr);
#if HMI_SIMULATE_SENSORS
    uint32_t lastSim = 0;
    float sim[3] = {25.8f, 58.0f, 180.0f};
    auto drift = [](float& v, float lo, float hi, float step) {
        v += ((float)random(-100, 101) / 100.0f) * step;
        if (v < lo) v = lo; if (v > hi) v = hi;
    };
#endif
    for (;;) {
        AppMsg m;
        while (xQueueReceive(g_appQ, &m, 0) == pdTRUE) {
            if (m.cmd == AppCmd::RelayToggle) setRelay(m.index, !g_relay[m.index].load());
            else if (m.cmd == AppCmd::RelaySet) setRelay(m.index, m.flag);
            else if (m.cmd == AppCmd::SaveBrightness) g_prefs.putUChar("bright", m.index);
            else if (m.cmd == AppCmd::SaveAutoDim) g_prefs.putBool("autodim", m.flag);
            else if (m.cmd == AppCmd::SaveTheme) g_prefs.putUChar("theme", m.index);
            else if (m.cmd == AppCmd::SaveDark) g_prefs.putBool("dark", m.flag);
            else if (m.cmd == AppCmd::SaveEspNow) {
                hmi::EspNowUiConfig ecfg{};
                if (g_espnowCfgQ && xQueueReceive(g_espnowCfgQ, &ecfg, 0) == pdTRUE) applyEspNowUiConfig(ecfg);
            }
        }
        EspCmd ec;
        while (xQueueReceive(g_espQ, &ec, 0) == pdTRUE) applyEsp(ec);

        const uint32_t now = millis();
#if HMI_SIMULATE_SENSORS
        if (now - lastSim > 2000) {
            lastSim = now;
            drift(sim[0], 22.0f, 33.0f, 0.4f); drift(sim[1], 40.0f, 80.0f, 1.5f); drift(sim[2], 60.0f, 450.0f, 12.0f);
            hmi::setPrimary(sim[0], hmi::SensorState::Ok);
            hmi::setMetric(0, sim[1], hmi::SensorState::Ok);
            hmi::setMetric(1, sim[2], hmi::SensorState::Ok);
        }
#endif
        // Rotate greeting on the header room title every 30 s, never repeat back-to-back.
        static uint32_t lastGreeting = 0;
        static int8_t lastIdx = -1;
        if (!lastGreeting || now - lastGreeting >= 30000) {
            lastGreeting = now;
            static const char* const greetings[] = {
                "Xin chào, chúc một ngày tốt lành",
                "Chào buổi sáng, hãy tràn đầy năng lượng",
                "Chào buổi chiều, một buổi thư giãn",
                "Chào buổi tối, nghỉ ngơi đầy đủ",
                "Hôm nay là một ngày mới",
                "Chúc sức khỏe và hạnh phúc",
                "Tận hưởng từng khoảnh khắc đẹp",
                "Hãy tin vào chính mình",
                "Mỗi giây đều có giá trị",
                "Chúc bạn luôn vui vẻ",
            };
            constexpr size_t kGreetCount = sizeof(greetings) / sizeof(greetings[0]);
            int8_t idx = random(0, (int)kGreetCount);
            while (kGreetCount > 1 && idx == lastIdx) idx = random(0, (int)kGreetCount);
            lastIdx = idx;
            hmi::setRoomName(greetings[idx]);
        }
        feedStatus(now);
        espnow::loop();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void consoleTask(void*) {
    esp_task_wdt_add(nullptr);
    char line[120]; uint8_t len = 0;
    for (;;) {
        while (Serial.available()) {
            const char c = Serial.read();
            if (c != '\n' && c != '\r') { if (len < sizeof(line) - 1) line[len++] = c; continue; }
            line[len] = 0; len = 0; if (!line[0]) continue;
            else if (!strcmp(line, "espnow")) espnow::print(Serial);
            else if (!strncmp(line, "espnow mac ", 11)) {
                storage::EspNowConfig ec = g_sdConfig.espnow;
                ec.enabled = true; ec.channel = ec.channel ? ec.channel : ESPNOW_CHANNEL;
                if (!ec.pmk[0]) strlcpy(ec.pmk, ESPNOW_PMK, sizeof(ec.pmk));
                if (!ec.lmk[0]) strlcpy(ec.lmk, ESPNOW_LMK, sizeof(ec.lmk));
                unsigned v[6];
                if (sscanf(line + 11, "%02x:%02x:%02x:%02x:%02x:%02x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) == 6) {
                    for (int i=0;i<6;i++) ec.peers[0][i]=(uint8_t)v[i]; ec.peerCount=1; g_sdConfig.espnow=ec;
                    bool ok = storage::saveEspNowConfig(ec);
                    espnow::begin(ec);
                    Serial.printf("ESP-NOW master MAC saved=%d %s (restart not required)\n", ok, line+11);
                } else Serial.println("usage: espnow mac AA:BB:CC:DD:EE:FF");
            }
            else if (!strcmp(line, "info")) Serial.printf("heap %u psram %u espnow %s\n", ESP.getFreeHeap(), ESP.getFreePsram(), espnow::statusText());
            else if (!strcmp(line, "touch mt")) {
                static GT911Touch gt911;
                if (gt911.begin()) {
                    g_mtGt911 = &gt911;
                    g_mtTestActive = true;
                    g_mtTestEndMs = millis() + 60000;
                    Serial.println("MT test: 60s. Touch screen with up to 5 fingers. 'touch stop' to end.");
                    Serial.println("Note: normal LVGL touch is paused during test.");
                } else {
                    Serial.println("MT test: GT911 init failed.");
                }
            }
            else if (!strcmp(line, "touch stop")) { g_mtTestActive = false; Serial.println("MT test stopped."); }
            else if (!strncmp(line, "toggle ", 7)) postApp(AppMsg{AppCmd::RelayToggle, (uint8_t)(atoi(line + 7) - 1), false});
            else Serial.printf("? %s\n", line);
        }
        // Multi-touch test: read all GT911 touch points and print to Serial.
        if (g_mtTestActive && g_mtGt911 && millis() < g_mtTestEndMs) {
            if (g_mtGt911->read()) {
                int cnt = g_mtGt911->getTouchCount();
                Serial.printf("MT %d points:", cnt);
                for (int i = 0; i < cnt; i++) {
                    auto p = g_mtGt911->getPoint(i);
                    if (p.valid) Serial.printf(" [%d:%d,%d sz=%d]", p.id, p.x, p.y, p.size);
                }
                Serial.println();
            }
        } else if (g_mtTestActive) {
            g_mtTestActive = false;
            g_mtGt911 = nullptr;
            Serial.println("MT test finished.");
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Forward declarations for OTA symbols used in netTask.
TaskHandle_t g_otaTask = nullptr;
bool g_otaRunning = false;
char g_autoOtaUrl[256] = "";   // URL for auto-OTA download (set by version check)
uint32_t g_lastOtaCheck = 0;   // last auto-OTA check time (ms)
void otaTask(void*);
bool checkOtaUpdate(char* outUrl, size_t outUrlSize);

void netTask(void*) {
    wifimgr::begin(g_sdConfig);
    esp_task_wdt_add(nullptr);
    for (;;) {
        NetMsg n;
        while (g_netQ && xQueueReceive(g_netQ, &n, 0) == pdTRUE) {
            if (n.cmd == NetCmd::Scan) wifimgr::startScan();
            else if (n.cmd == NetCmd::Join) wifimgr::join(n.ssid, n.pass);
        }
        wifimgr::loop();
        esp_task_wdt_reset();

        // Auto OTA: when Wi-Fi connected, check for updates periodically.
        if (OTA_AUTO_UPDATE && wifimgr::connected() && !g_otaRunning) {
            uint32_t now = millis();
            if (now - g_lastOtaCheck >= OTA_CHECK_INTERVAL_MS || g_lastOtaCheck == 0) {
                g_lastOtaCheck = now;
                char newUrl[256] = "";
                if (checkOtaUpdate(newUrl, sizeof(newUrl))) {
                    strlcpy(g_autoOtaUrl, newUrl, sizeof(g_autoOtaUrl));
                    Serial.println("OTA: auto-update starting...");
                    xTaskCreatePinnedToCore(otaTask, "ota", 8 * 1024, nullptr, 2, &g_otaTask, 0);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void onRelayToggle(uint8_t i) { postApp(AppMsg{AppCmd::RelayToggle, i, false}); }
void onBrightnessChanged(uint8_t pct) { postApp(AppMsg{AppCmd::SaveBrightness, pct, false}); }
void onAutoDimChanged(bool on) { postApp(AppMsg{AppCmd::SaveAutoDim, 0, on}); }
void onThemeChanged(uint8_t i) { postApp(AppMsg{AppCmd::SaveTheme, i, false}); }
void onDarkModeChanged(bool on) { postApp(AppMsg{AppCmd::SaveDark, 0, on}); }
void onBacklight(uint8_t pct) { pct = constrain(pct, 0, 100); display.setBrightness(map(pct, 0, 100, HMI_BACKLIGHT_MIN_DUTY, 255)); }
void onWifiScan() { if (g_netQ) { NetMsg m{NetCmd::Scan, {}, {}}; xQueueSend(g_netQ, &m, 0); } }
void onWifiJoin(const char* ssid, const char* pass) {
    if (!g_netQ) return;
    NetMsg m{NetCmd::Join, {}, {}};
    strlcpy(m.ssid, ssid ? ssid : "", sizeof(m.ssid));
    strlcpy(m.pass, pass ? pass : "", sizeof(m.pass));
    xQueueSend(g_netQ, &m, 0);
}

hmi::EspNowUiConfig espNowUiConfig() {
    hmi::EspNowUiConfig c{};
    c.enabled = g_sdConfig.espnow.enabled;
    c.channel = g_sdConfig.espnow.channel ? g_sdConfig.espnow.channel : ESPNOW_CHANNEL;
    strlcpy(c.pmk, g_sdConfig.espnow.pmk, sizeof(c.pmk));
    strlcpy(c.lmk, g_sdConfig.espnow.lmk, sizeof(c.lmk));
    if (g_sdConfig.espnow.peerCount > 0) {
        const uint8_t* m = g_sdConfig.espnow.peers[0];
        snprintf(c.masterMac, sizeof(c.masterMac), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        if (!strcmp(c.masterMac, "00:00:00:00:00:00")) c.masterMac[0] = 0;
    }
    if (!c.masterMac[0]) strlcpy(c.masterMac, ESPNOW_MASTER_MAC, sizeof(c.masterMac));
    return c;
}

bool parseMacUi(const char* text, uint8_t mac[6]) {
    unsigned v[6] = {};
    if (!text) return false;
    if (sscanf(text, "%02x:%02x:%02x:%02x:%02x:%02x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6 &&
        sscanf(text, "%02X:%02X:%02X:%02X:%02X:%02X", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return true;
}

void applyEspNowUiConfig(const hmi::EspNowUiConfig& ui) {
    storage::EspNowConfig ec = g_sdConfig.espnow;
    ec.enabled = ui.enabled;
    ec.channel = ui.channel ? ui.channel : ESPNOW_CHANNEL;
    if (ui.pmk[0]) strlcpy(ec.pmk, ui.pmk, sizeof(ec.pmk));
    if (ui.lmk[0]) strlcpy(ec.lmk, ui.lmk, sizeof(ec.lmk));
    if (!ec.pmk[0]) strlcpy(ec.pmk, ESPNOW_PMK, sizeof(ec.pmk));
    if (!ec.lmk[0]) strlcpy(ec.lmk, ESPNOW_LMK, sizeof(ec.lmk));
    uint8_t mac[6];
    if (parseMacUi(ui.masterMac, mac)) {
        for (int i = 0; i < 6; i++) ec.peers[0][i] = mac[i];
        ec.peerCount = 1;
    }
    g_sdConfig.espnow = ec;
    const bool ok = storage::saveEspNowConfig(ec);
    espnow::begin(ec);
    hmi::setEspNowUiConfig(espNowUiConfig());
    storage::logEvent("ESP-NOW config from UI saved=%d mac=%s ch=%u", ok, ui.masterMac, ec.channel);
    Serial.printf("ESP-NOW config saved=%d mac=%s ch=%u\n", ok, ui.masterMac, ec.channel);
}

void onEspNowConfigSave(const hmi::EspNowUiConfig& cfg) {
    g_pendingEspNowUi = cfg;
    if (g_espnowCfgQ) xQueueSend(g_espnowCfgQ, &g_pendingEspNowUi, 0);
    postApp(AppMsg{AppCmd::SaveEspNow, 0, false});
}


// ---------- OTA firmware update (HTTP -> OTA partition) ----------

static void otaProgressCb(int pct) {
    hmi::setUpdateProgress(static_cast<int8_t>(pct));
}

// Compare two semantic version strings (e.g. "1.1.0-ui" vs "1.2.0").
// Returns 1 if a > b, -1 if a < b, 0 if equal.
static int versionCompare(const char* a, const char* b) {
    while (*a || *b) {
        int na = 0, nb = 0;
        while (*a && *a >= '0' && *a <= '9') { na = na * 10 + (*a - '0'); a++; }
        while (*b && *b >= '0' && *b <= '9') { nb = nb * 10 + (*b - '0'); b++; }
        if (na < nb) return -1;
        if (na > nb) return 1;
        if (*a) a++;
        if (*b) b++;
        while (*a && *a < '0' && *a != '\0') a++;
        while (*b && *b < '0' && *b != '\0') b++;
    }
    return 0;
}

// Fetch version manifest from OTA_UPDATE_URL. If a newer version is available,
// copies the download URL to outUrl and returns true.
// Manifest: {"version":"1.2.0","url":"https://.../firmware.bin","forced":false}
bool checkOtaUpdate(char* outUrl, size_t outUrlSize) {
    const char* url = OTA_UPDATE_URL;
    if (!url || !url[0]) return false;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    int64_t contentLen = esp_http_client_get_content_length(client);
    if (status < 200 || status >= 300 || contentLen <= 0 || contentLen > 2048) {
        esp_http_client_cleanup(client);
        return false;
    }

    char* buf = (char*)heap_caps_malloc(contentLen + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        esp_http_client_cleanup(client);
        return false;
    }

    int totalRead = 0;
    while (totalRead < contentLen) {
        int read = esp_http_client_read(client, buf + totalRead, contentLen - totalRead);
        if (read <= 0) break;
        totalRead += read;
    }
    buf[totalRead] = 0;
    esp_http_client_cleanup(client);

    ArduinoJson::JsonDocument doc;
    auto err2 = deserializeJson(doc, buf);
    heap_caps_free(buf);
    if (err2) return false;

    const char* newVersion = doc["version"] | "";
    const char* newUrl = doc["url"] | "";
    bool forced = doc["forced"] | false;

    if (!newVersion[0] || !newUrl[0]) return false;

    int cmp = versionCompare(newVersion, FIRMWARE_VERSION);
    if (cmp <= 0 && !forced) return false;  // not newer and not forced

    strlcpy(outUrl, newUrl, outUrlSize);
    Serial.printf("OTA: new version %s available (current %s), url=%s\n", newVersion, FIRMWARE_VERSION, newUrl);
    return true;
}

void otaTask(void*) {
    const char* url = g_autoOtaUrl[0] ? g_autoOtaUrl : OTA_FIRMWARE_URL;
    if (!url || !url[0]) {
        Serial.println("OTA: OTA_FIRMWARE_URL not set");
        hmi::setUpdateProgress(-1);
        g_otaRunning = false;
        vTaskDelete(nullptr);
    }

    if (wifimgr::connected() == false) {
        Serial.println("OTA: Wi-Fi not connected");
        hmi::setUpdateProgress(-1);
        g_otaRunning = false;
        vTaskDelete(nullptr);
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    cfg.keep_alive_enable = true;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        Serial.println("OTA: failed to init http client");
        hmi::setUpdateProgress(-1);
        g_otaRunning = false;
        vTaskDelete(nullptr);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        Serial.printf("OTA: http open failed %s\n", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        hmi::setUpdateProgress(-1);
        g_otaRunning = false;
        vTaskDelete(nullptr);
    }

    int status = esp_http_client_get_status_code(client);
    int64_t contentLen = esp_http_client_get_content_length(client);
    Serial.printf("OTA: HTTP %d contentLen=%lld\n", status, (long long)contentLen);
    if (status < 200 || status >= 300) {
        esp_http_client_cleanup(client);
        hmi::setUpdateProgress(-1);
        g_otaRunning = false;
        vTaskDelete(nullptr);
    }

    // Sao chép firmware đang chạy vào /backup trước khi ghi OTA, để có thể khôi phục nếu cần.
    storage::backupRunningFirmware();

    const esp_partition_t* otaPart = esp_ota_get_next_update_partition(nullptr);
    if (!otaPart) {
        Serial.println("OTA: no OTA partition");
        esp_http_client_cleanup(client);
        hmi::setUpdateProgress(-1);
        g_otaRunning = false;
        vTaskDelete(nullptr);
    }

    esp_ota_handle_t otaHandle = 0;
    err = esp_ota_begin(otaPart, OTA_SIZE_UNKNOWN, &otaHandle);
    if (err != ESP_OK) {
        Serial.printf("OTA: ota_begin failed %s\n", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        hmi::setUpdateProgress(-1);
        g_otaRunning = false;
        vTaskDelete(nullptr);
    }

    char buf[1024];
    int64_t totalWritten = 0;
    int lastPct = -1;
    bool ok = true;

    while (true) {
        int read = esp_http_client_read(client, buf, sizeof(buf));
        if (read < 0) {
            Serial.printf("OTA: read failed %s\n", esp_err_to_name(static_cast<esp_err_t>(read)));
            ok = false;
            break;
        }
        if (read == 0) break;

        err = esp_ota_write(otaHandle, buf, read);
        if (err != ESP_OK) {
            Serial.printf("OTA: ota_write failed %s\n", esp_err_to_name(err));
            ok = false;
            break;
        }

        totalWritten += read;
        if (contentLen > 0) {
            int pct = static_cast<int>((totalWritten * 100) / contentLen);
            if (pct != lastPct) {
                lastPct = pct;
                otaProgressCb(pct);
                Serial.printf("OTA: %d%%\n", pct);
            }
        }
        esp_task_wdt_reset();
    }

    if (ok) {
        err = esp_ota_end(otaHandle);
        if (err != ESP_OK) {
            Serial.printf("OTA: ota_end failed %s\n", esp_err_to_name(err));
            ok = false;
        } else {
            err = esp_ota_set_boot_partition(otaPart);
            if (err != ESP_OK) {
                Serial.printf("OTA: set_boot_partition failed %s\n", esp_err_to_name(err));
                ok = false;
            }
        }
    }

    if (ok) {
        otaProgressCb(100);
        Serial.println("OTA: success, rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        esp_ota_abort(otaHandle);
        hmi::setUpdateProgress(-1);
        Serial.println("OTA: failed");
        g_autoOtaUrl[0] = 0;  // clear so next check re-reads the manifest
    }

    esp_http_client_cleanup(client);
    g_otaRunning = false;
    vTaskDelete(nullptr);
}

void onFirmwareUpdate() {
    if (g_otaRunning) return;
    if (!OTA_FIRMWARE_URL[0]) {
        Serial.println("OTA: disabled (no URL)");
        hmi::setUpdateProgress(-1);
        return;
    }
    if (!wifimgr::connected()) {
        Serial.println("OTA: Wi-Fi not connected");
        hmi::setUpdateProgress(-1);
        return;
    }
    hmi::setUpdateProgress(0);
    g_otaRunning = true;
    xTaskCreatePinnedToCore(otaTask, "ota", 8 * 1024, nullptr, 2, &g_otaTask, 0);
}

}  // namespace

void setup() {
    Serial.begin(SERIAL_BAUDRATE);
    delay(100);
    Serial.printf("\n=== ESP32-4848S040 ESP-NOW Slave Panel ===\nFW %s reset=%d\n", FIRMWARE_VERSION, (int)esp_reset_reason());
    g_bootTime = millis();

    for (int i = 0; i < RELAY_COUNT; i++) { pinMode(kRelayPins[i], OUTPUT); digitalWrite(kRelayPins[i], LOW); g_relay[i] = false; g_relayChange[i] = millis(); }

    if (!display.begin()) Serial.println("!!! display.begin() FAILED !!!");
    display.setBrightness(255);

    datalog::begin();
    datalog::addEvent("Hệ thống khởi động");
    storage::begin();
    storage::loadConfig(g_sdConfig);
    g_sdConfig.espnow.enabled = true;
    if (!g_sdConfig.espnow.channel) g_sdConfig.espnow.channel = ESPNOW_CHANNEL;
    if (!g_sdConfig.espnow.pmk[0]) strlcpy(g_sdConfig.espnow.pmk, ESPNOW_PMK, sizeof(g_sdConfig.espnow.pmk));
    if (!g_sdConfig.espnow.lmk[0]) strlcpy(g_sdConfig.espnow.lmk, ESPNOW_LMK, sizeof(g_sdConfig.espnow.lmk));
    if (g_sdConfig.espnow.peerCount == 0) g_sdConfig.espnow.peerCount = 1;  // parsed from ESPNOW_MASTER_MAC in begin()

    g_prefs.begin("hmi", false);
    hmi::setBrightness(g_prefs.getUChar("bright", 80));
    hmi::setAutoDim(g_prefs.getBool("autodim", true));
    hmi::setTheme(g_prefs.getUChar("theme", 0));
    hmi::setDarkMode(g_prefs.getBool("dark", true));
    hmi::setLink(hmi::Link::Connecting);

    esp_task_wdt_init(10, true);
    g_appQ = xQueueCreate(24, sizeof(AppMsg));
    g_espQ = xQueueCreate(24, sizeof(EspCmd));
    g_espnowCfgQ = xQueueCreate(4, sizeof(hmi::EspNowUiConfig));
    g_netQ = xQueueCreate(8, sizeof(NetMsg));
    espnow::setCommandQueue(g_espQ);

    hmi::Callbacks cb{};
    cb.relayToggle = onRelayToggle; cb.brightnessChanged = onBrightnessChanged; cb.autoDimChanged = onAutoDimChanged;
    cb.themeChanged = onThemeChanged; cb.darkModeChanged = onDarkModeChanged; cb.backlight = onBacklight;
    cb.wifiScan = onWifiScan; cb.wifiJoin = onWifiJoin;
    cb.espnowConfigSave = onEspNowConfigSave;
    cb.firmwareUpdate = onFirmwareUpdate;
    hmi::setEspNowUiConfig(espNowUiConfig());
    hmi::begin(display, cb);
    storage::start();

    xTaskCreatePinnedToCore(netTask, "net", 8 * 1024, nullptr, 2, nullptr, 0);
    espnow::begin(g_sdConfig.espnow);
    espnow::loop();

    xTaskCreatePinnedToCore(appTask, "app", 5 * 1024, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(consoleTask, "console", 3 * 1024, nullptr, 1, nullptr, 0);
    Serial.printf("Ready: Wi-Fi manager + ESP-NOW slave. master=%s\n", ESPNOW_MASTER_MAC);
}

void loop() { vTaskDelete(nullptr); }
