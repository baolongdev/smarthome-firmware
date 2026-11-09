#pragma once
// Tasmota relays, driven through the web gateway (HTTPS, works from any
// network with Internet) in the "devices" FreeRTOS task (core 0), so a slow
// server never delays MQTT or the UI.
//
//   poll:   GET  <api>  -> {"states":{"primary":{"power1":true,...},"secondary":{...}}}
//   toggle: POST <api> {"device":"primary","channel":1,"action":"toggle"}
//
// State is pushed to the HMI with hmi::setDeviceState(); online/offline
// transitions and toggles are logged to the SD card.

#include "hmi.h"

namespace tasmota {

// Device list: from /config.json if it has "tasmota", else config.h.
// apiUrl: web gateway (TASMOTA_API_URL or /config.json "tasmotaApi").
void start(const hmi::DeviceInfo* list, uint8_t count, const char* apiUrl);
void toggle(uint8_t device, uint8_t channel);  // any task, non-blocking
void print(Print& out);                        // console "devices"

}  // namespace tasmota
