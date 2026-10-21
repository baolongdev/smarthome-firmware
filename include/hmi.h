#pragma once
// Public API of the LVGL HMI.  Firmware only pushes state in and receives user
// intents through callbacks; it never touches LVGL objects.
//
//   hmi::begin(display, callbacks);   // starts the "ui" FreeRTOS task (core 1)
//   hmi::setRelay(0, true); ...       // from ANY task: locked, cheap (marks state dirty)
//
// Callbacks run inside the UI task: they must return quickly (post to a queue,
// never block on network/SD), or the UI freezes and the task watchdog fires.

#include <Arduino.h>

class LGFX;

namespace hmi {

enum class SensorState : uint8_t { Loading, Ok, Error, Stale };
enum class Link : uint8_t { Connecting, Online, BrokerDown, Offline };

// Everything the Settings rows and their detail pages show.  Sent by the net
// task every few seconds.
struct SystemInfo {
    // network
    char ssid[33] = "";
    char ip[16] = "";
    int rssi = 0;
    char bssid[18] = "";
    uint8_t channel = 0;
    char gateway[16] = "";
    char subnet[16] = "";
    char dns[16] = "";
    char mac[18] = "";
    char hostname[32] = "";
    // MQTT
    char mqttHost[64] = "";
    uint16_t mqttPort = 0;
    int8_t mqttState = -1;    // PubSubClient::state()
    bool mqttAuth = false;    // user name configured
    // memory
    uint32_t heapKB = 0;
    uint32_t heapMinKB = 0;
    uint32_t heapTotalKB = 0;
    uint32_t heapLargestKB = 0;
    uint32_t psramKB = 0;
    uint32_t psramTotalKB = 0;
    uint32_t flashMB = 0;
    uint32_t appKB = 0;       // running firmware size
    uint32_t appSlotKB = 0;   // free OTA slot
    // runtime / chip
    uint32_t uptimeS = 0;
    uint8_t resetReason = 0;  // esp_reset_reason()
    bool timeSynced = false;
    char date[11] = "";       // "13/10/2026" once NTP synced
    char chip[24] = "";
    uint8_t chipRev = 0;
    uint8_t cores = 0;
    uint16_t cpuMHz = 0;
    char sdk[24] = "";
};

struct WifiNetwork {
    char ssid[33];
    int8_t rssi;
    bool secure;
    bool saved = false;  // password already known (NVS, config.json or config.h)
};

enum class WifiJoin : uint8_t { Idle, Connecting, Connected, Failed };

struct StorageInfo {
    bool present = false;
    uint32_t totalMB = 0;
    uint32_t usedMB = 0;
    bool configLoaded = false;  // /config.json applied at boot
    uint32_t firmwareKB = 0;    // /firmware.bin size, 0 = none
    char cardType[8] = "";      // "SDHC", "SD", "MMC"
    uint32_t spiKHz = 0;        // mount speed
    // /config.json summary
    char cfgRoom[32] = "";
    uint8_t cfgWifi = 0;
    char cfgMqtt[64] = "";
    uint8_t cfgTasmota = 0;
    uint8_t configSource = 0;   // 0 none, 1 SD card, 2 flash copy (LittleFS)
    uint16_t logDays = 0;       // sensor CSV retention
    uint32_t backupKB = 0;      // /backup/firmware-prev.bin, 0 = none
    char backupVer[24] = "";
    uint32_t lfsTotalKB = 0;    // LittleFS on the flash "spiffs" partition
    uint32_t lfsUsedKB = 0;
};

// Network devices (Tasmota) shown on the Control screen, grouped per device.
constexpr uint8_t kMaxDevices = 4;
constexpr uint8_t kMaxChannels = 8;
struct DeviceInfo {
    char name[24];
    char key[16];   // device name on the web gateway: "primary", "secondary"
    uint8_t channels;
    char labels[kMaxChannels][20];  // "" = "Kênh n"
};

// SD card file browser (filled by the storage task).
constexpr uint8_t kMaxFiles = 32;
constexpr uint16_t kImageMax = 432;  // image preview fits the 432 px sub-page column
struct FileEntry {
    char name[40];
    uint32_t size;
    bool dir;
};
struct FileList {
    char path[96] = "/";
    uint8_t count = 0;
    bool truncated = false;     // more than kMaxFiles entries
    bool loading = false;
    char error[48] = "";
    FileEntry items[kMaxFiles];
};
struct FileView {
    char path[96] = "";
    uint32_t size = 0;
    bool loading = false;
    bool previewable = false;   // text file: `text` holds (part of) it
    bool tail = false;          // `text` is the end of a longer file
    uint32_t textLen = 0;       // bytes of text, kept outside the model (up to 64 KB)
    bool image = false;         // image file: imgW x imgH RGB565 pixels outside the model
    uint16_t imgW = 0, imgH = 0;  // fitted into kImageMax x kImageMax
    uint16_t srcW = 0, srcH = 0;  // size of the image in the file
    char error[48] = "";
};

struct Callbacks {
    void (*relayToggle)(uint8_t index) = nullptr;     // user tapped a relay row
    void (*brightnessChanged)(uint8_t pct) = nullptr; // slider released (persist it)
    void (*autoDimChanged)(bool on) = nullptr;        // Settings switch (persist it)
    void (*themeChanged)(uint8_t index) = nullptr;    // Settings colour palette (persist it)
    void (*wifiScan)() = nullptr;                     // Wi-Fi screen opened / refresh tapped
    void (*wifiJoin)(const char* ssid, const char* password) = nullptr;
    void (*firmwareUpdate)() = nullptr;               // SD update confirmed (tapped twice)
    void (*deviceToggle)(uint8_t device, uint8_t channel) = nullptr;
    void (*fileList)(const char* path) = nullptr;     // open / refresh a folder
    void (*fileRead)(const char* path) = nullptr;     // open a file
    void (*fileDelete)(const char* path) = nullptr;   // confirmed (tapped twice)
    void (*backlight)(uint8_t pct) = nullptr;         // drive the panel backlight
};

void configureDevices(const DeviceInfo* list, uint8_t count);  // before begin()
void begin(LGFX& lcd, const Callbacks& cb);

void setPrimary(float value, SensorState state = SensorState::Ok);
void setMetric(uint8_t index, float value, SensorState state = SensorState::Ok);  // 0..1
void setLink(Link link);
void setClock(const char* hhmm);  // "--:--" until NTP
void setRelay(uint8_t index, bool on);
void setBrightness(uint8_t pct);  // 0..100 slider scale (0 % is still half backlight)
void setAutoDim(bool on);
void setTheme(uint8_t index);  // colour palette: 0 Lime, 1 Ember, 2 Cobalt
void setSystemInfo(const SystemInfo& info);
void setRoomName(const char* name);  // header title (default HMI_ROOM_NAME)

void setWifiNetworks(const WifiNetwork* list, uint8_t count);  // scan result
void setWifiScanning(bool scanning);
void setWifiJoin(WifiJoin state, const char* message);         // Connected: IP, Failed: reason
void setStorage(const StorageInfo& info);
void setUpdateProgress(int8_t pct);
// Bit n of `mask` = channel n+1 on.  `online` false greys the group out.
void setDeviceState(uint8_t device, uint8_t mask, bool online);
void setFileList(const FileList& list);
// text / pixels (native RGB565, view.imgW x view.imgH) are copied into PSRAM.
void setFileView(const FileView& view, const char* text = nullptr, const uint16_t* pixels = nullptr);  // 0..100 shows the update overlay, -1 hides it

void showPage(uint8_t page);  // 0 Tổng quan, 1 Điều khiển, 2 Cài đặt, 3 Wi-Fi (async)
void scrollPage(int16_t dy);  // scroll the current page by dy px (console testing)
void previewKeyboard(bool open = false);  // open the Wi-Fi password sheet for a dummy network (console testing)
void showDetail(uint8_t topic);  // Settings detail page: 0 Mạng .. 2 Bộ nhớ .. 6 Giới thiệu (console)
// Rebuild the UI and time full redraws; policy 0 keep, 1 all-PSRAM, 2 hybrid (console).
void benchmark(uint8_t policy);
void previewFile(const char* path);  // open a file in the viewer (console "view <path>")
// Synthetic touch drag through the real LVGL input path (console testing).
void injectDrag(int16_t x, int16_t y0, int16_t y1, uint16_t ms);

// Screenshots: the UI task takes the snapshot, the calling task streams it.
// RLE RGB565 dump of the current screen for tools/screenshot.py.
void screenshot(Print& out);
// Same format, from the panel framebuffer `delayMs` from now (console "shot fb <ms>").
void screenshotFramebuffer(Print& out, uint32_t delayMs);
// 24-bit BMP of the current screen (e.g. to a File on the SD card).
bool screenshotBmp(Print& out);

}  // namespace hmi
