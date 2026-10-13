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

struct SystemInfo {
    char ssid[33] = "";
    char ip[16] = "";
    int rssi = 0;
    uint32_t heapKB = 0;
    uint32_t psramKB = 0;
    uint32_t uptimeS = 0;
};

struct WifiNetwork {
    char ssid[33];
    int8_t rssi;
    bool secure;
};

enum class WifiJoin : uint8_t { Idle, Connecting, Connected, Failed };

struct StorageInfo {
    bool present = false;
    uint32_t totalMB = 0;
    uint32_t usedMB = 0;
    bool configLoaded = false;  // /config.json applied at boot
    uint32_t firmwareKB = 0;    // /firmware.bin size, 0 = none
};

// Network devices (Tasmota) shown on the Control screen, grouped per device.
constexpr uint8_t kMaxDevices = 4;
constexpr uint8_t kMaxChannels = 8;
struct DeviceInfo {
    char name[24];
    char host[16];
    uint8_t channels;
    char labels[kMaxChannels][20];  // "" = "Kênh n"
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
    void (*backlight)(uint8_t pct) = nullptr;         // drive the panel backlight
};

void configureDevices(const DeviceInfo* list, uint8_t count);  // before begin()
void begin(LGFX& lcd, const Callbacks& cb);

void setPrimary(float value, SensorState state = SensorState::Ok);
void setMetric(uint8_t index, float value, SensorState state = SensorState::Ok);  // 0..1
void setLink(Link link);
void setClock(const char* hhmm);  // "--:--" until NTP
void setRelay(uint8_t index, bool on);
void setBrightness(uint8_t pct);  // 10..100
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
void setDeviceState(uint8_t device, uint8_t mask, bool online);  // 0..100 shows the update overlay, -1 hides it

void showPage(uint8_t page);  // 0 Tổng quan, 1 Điều khiển, 2 Cài đặt, 3 Wi-Fi (async)
void scrollPage(int16_t dy);  // scroll the current page by dy px (console testing)

// Screenshots: the UI task takes the snapshot, the calling task streams it.
// RLE RGB565 dump of the current screen for tools/screenshot.py.
void screenshot(Print& out);
// 24-bit BMP of the current screen (e.g. to a File on the SD card).
bool screenshotBmp(Print& out);

}  // namespace hmi
