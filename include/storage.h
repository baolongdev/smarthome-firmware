#pragma once
// microSD (TF) card service, running as the "storage" FreeRTOS task.
// Vendor pin map (Docs/.../IO pin distribution.xlsx): CS=42, MISO=41, SCK=48,
// MOSI=47.  SCK/MOSI are shared with the ST7701 init bus, which is only used
// inside display.begin(), so mount after the display.
//
// Only the storage task touches the card after start(): other tasks enqueue
// work (never block), so slow FAT operations cannot stall the UI.
//
// Card layout (all optional; the panel works without a card):
//   /config.json                 Wi-Fi list, MQTT broker, room, Tasmota devices (read at boot)
//   /firmware.bin                offered in Settings, flashed on confirm
//   /logs/events.log             boot, relay, network events (rotates at 256 KB)
//   /logs/sensors/YYYY-MM-DD.csv one row per minute, kept "logDays" days (default 90)
//   /data/history.bin            sensor history rings (datalog), saved every 10 min
//   /backup/firmware-prev.bin    running image saved before each SD update ("sd restore")
//   /screenshots/shot_NNN.bmp    serial command "shot sd"
//
// Flash (LittleFS on the 3.4 MB "spiffs" partition, mounted at /lfs):
//   /config.json                 copy of the last valid SD config (used without a card)
//   /history.bin                 sensor history, saved every hour

#include <Arduino.h>

#include "hmi.h"

namespace storage {

constexpr uint8_t kMaxWifi = 4;

struct Config {
    bool loaded = false;
    uint8_t wifiCount = 0;
    char wifiSsid[kMaxWifi][33] = {};
    char wifiPass[kMaxWifi][65] = {};
    char mqttHost[64] = "";
    uint16_t mqttPort = 0;
    char mqttUser[32] = "";
    char mqttPass[64] = "";
    char room[32] = "";
    uint8_t tasmotaCount = 0;  // 0 = use the config.h devices
    hmi::DeviceInfo tasmota[hmi::kMaxDevices] = {};
};

// --- setup(), before start(): synchronous ---
bool begin();                  // mount if a card is present (safe without one)
bool loadConfig(Config& out);  // /config.json
void start();                  // spawn the storage task (core 0, priority 1)

// --- any task: non-blocking (dropped if the queue is full) ---
// "<time> <message>" to /logs/events.log, echoed to Serial immediately.
void logEvent(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logSensors(float primary, float metric1, float metric2);
void requestFirmwareUpdate();  // flash /firmware.bin, restart on success
void requestScreenshot();      // BMP of the current screen to /screenshots
// File browser: results go to hmi::setFileList / hmi::setFileView.
void requestList(const char* dir);
void requestRead(const char* path);    // text files: whole file or its last 64 KB
void requestDelete(const char* path);  // files and empty folders; re-lists the parent
void requestTree();  // print every folder and file to Serial (console "sd tree")
void requestWipe();  // delete everything on the card, keep /logs (console "sd wipe")
void requestRestore();      // reinstall /backup/firmware-prev.bin (console "sd restore")
void requestHistorySave();  // save history to SD and flash now (console "history save")

}  // namespace storage
