#pragma once
// Wi-Fi business logic: known networks, async scan, user join with result.
// Pushes state to the HMI (hmi::setWifiNetworks / setWifiJoin).
//
// Known networks, tried in order until one connects:
//   1. the last network joined from the panel (NVS "wifi"/ssid,pass)
//   2. /config.json "wifi" list on the SD card
//   3. WIFI_SSID / WIFI_PASSWORD from config.h

#include "storage.h"

namespace wifimgr {

void begin(const storage::Config& sd);
void loop();
void startScan();
void join(const char* ssid, const char* password);
bool connected();

}  // namespace wifimgr
