#pragma once
// Wi-Fi business logic: known networks, async scan, user join with result.
// Pushes state to the HMI (hmi::setWifiNetworks / setWifiJoin).
//
// Known networks, tried in order until one connects:
//   1. the last network joined from the panel (NVS "wifi"/ssid,pass)
//   2. /config.json "wifi" list on the SD card
//   3. WIFI_SSID / WIFI_PASSWORD from config.h

#include "hmi.h"
#include "storage.h"

namespace wifimgr {

void begin(const storage::Config& sd);
void loop();
void startScan();
// password kUseSaved: use the stored one (saved network); "" for open networks.
constexpr const char* kUseSaved = "\x01";
void join(const char* ssid, const char* password);
// Remove a saved network (NVS + runtime list). Stops auto-joining it; if it is
// the active connection the panel is disconnected and the next known network
// is tried.
void forget(const char* ssid);
// Enable/disable auto-join for a saved network (NVS "auto:<ssid>").
void setAutoJoin(const char* ssid, bool on);
// Current saved list in priority order (first entry = preferred).
uint8_t savedList(hmi::SavedNetwork* out, uint8_t maxCount);
bool connected();
bool busy();  // scanning/connecting; ESP-NOW channel scanning must pause while true
void joinBssid(const char* ssid, const uint8_t* bssid, int32_t channel);  // diagnostics
void retryWithNewMac(const char* ssid);
void retry11b(const char* ssid);  // diagnostics: 802.11b only until reboot  // diagnostics: other MAC until reboot

}  // namespace wifimgr
