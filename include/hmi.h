#pragma once
// Public API of the LVGL HMI.  Firmware only pushes state in and receives user
// intents through callbacks; it never touches LVGL objects.

#include <Arduino.h>

class LGFX;

namespace hmi {

enum class SensorState : uint8_t { Loading, Ok, Error, Stale };
enum class Link : uint8_t { Connecting, Online, BrokerDown, Offline };

struct WifiNetwork {
    char ssid[33];
    int8_t rssi;
    bool secure;
    bool saved = false;  // password already known (NVS, config.json or config.h)
};

enum class WifiJoin : uint8_t { Idle, Connecting, Connected, Failed };

// Everything the Settings rows and their detail pages show.  Sent by the net
// task every few seconds.
struct SystemInfo {
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
    char mqttHost[64] = "";
    uint8_t espnowNodes = 0;
    bool espnowReady = false;
    char espnowStatus[40] = "";
    uint16_t mqttPort = 0;
    int8_t mqttState = -1;
    bool mqttAuth = false;
    uint32_t heapKB = 0;
    uint32_t heapMinKB = 0;
    uint32_t heapTotalKB = 0;
    uint32_t heapLargestKB = 0;
    uint32_t psramKB = 0;
    uint32_t psramTotalKB = 0;
    uint32_t flashMB = 0;
    uint32_t appKB = 0;
    uint32_t appSlotKB = 0;
    uint32_t uptimeS = 0;
    uint8_t resetReason = 0;
    bool timeSynced = false;
    char date[11] = "";
    char chip[24] = "";
    uint8_t chipRev = 0;
    uint8_t cores = 0;
    uint16_t cpuMHz = 0;
    char sdk[24] = "";
};

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

constexpr uint8_t kMaxDevices = 4;
constexpr uint8_t kMaxChannels = 8;
struct DeviceInfo { char name[24]; char key[16]; uint8_t channels; char labels[kMaxChannels][20]; };

constexpr uint8_t kMaxFiles = 32;
constexpr uint16_t kImageMax = 432;
struct FileEntry { char name[40]; uint32_t size; bool dir; };
struct FileList { char path[96] = "/"; uint8_t count = 0; bool truncated = false; bool loading = false; char error[48] = ""; FileEntry items[kMaxFiles]; };
struct FileView { char path[96] = ""; bool loading = false; char error[48] = ""; uint32_t size = 0; uint16_t textLen = 0; uint16_t srcW = 0, srcH = 0; bool truncated = false; bool previewable = false; const char* tail = nullptr; bool image = false; uint16_t imgW = 0, imgH = 0; };

struct EspNowUiConfig {
    char masterMac[18] = "";
    char pmk[33] = "";
    char lmk[33] = "";
    uint8_t channel = 1;
    bool enabled = true;
};

struct Callbacks {
    void (*relayToggle)(uint8_t index) = nullptr;
    void (*brightnessChanged)(uint8_t pct) = nullptr;
    void (*autoDimChanged)(bool on) = nullptr;
    void (*themeChanged)(uint8_t index) = nullptr;
    void (*darkModeChanged)(bool on) = nullptr;
    void (*backlight)(uint8_t pct) = nullptr;
    void (*firmwareUpdate)() = nullptr;
    void (*deviceToggle)(uint8_t device, uint8_t channel) = nullptr;
    void (*fileList)(const char* path) = nullptr;
    void (*fileRead)(const char* path) = nullptr;
    void (*fileDelete)(const char* path) = nullptr;
    void (*wifiScan)() = nullptr;                     // Wi-Fi screen opened / refresh tapped
    void (*wifiJoin)(const char* ssid, const char* password) = nullptr;
    void (*espnowConfigSave)(const EspNowUiConfig& cfg) = nullptr;
};

void configureDevices(const DeviceInfo* list, uint8_t count);
void begin(LGFX& lcd, const Callbacks& cb);

void setPrimary(float value, SensorState state = SensorState::Ok);
void setMetric(uint8_t index, float value, SensorState state = SensorState::Ok);
void setLink(Link link);
void setClock(const char* hhmm);
void setRelay(uint8_t index, bool on);
void setBrightness(uint8_t pct);
void setAutoDim(bool on);
void setTheme(uint8_t index);
void setDarkMode(bool on);
void setSystemInfo(const SystemInfo& info);
void setEspNowUiConfig(const EspNowUiConfig& cfg);
void setWifiNetworks(const WifiNetwork* list, uint8_t count);  // scan result
void setWifiScanning(bool scanning);
void setWifiJoin(WifiJoin state, const char* message);         // Connected: IP, Failed: reason
void setRoomName(const char* name);

void setStorage(const StorageInfo& info);
void setUpdateProgress(int8_t pct);
void setDeviceState(uint8_t device, uint8_t mask, bool online);
void setFileList(const FileList& list);
void setFileView(const FileView& view, const char* text = nullptr, const uint16_t* pixels = nullptr);

void showPage(uint8_t page);
void scrollPage(int16_t dy);
void previewKeyboard(bool open = false);
void showDetail(uint8_t topic);
void benchmark(uint8_t policy);
void previewFile(const char* path);
void injectDrag(int16_t x, int16_t y0, int16_t y1, uint16_t ms);
void screenshot(Print& out);
void screenshotFramebuffer(Print& out, uint32_t delayMs);
bool screenshotBmp(Print& out);

}  // namespace hmi
