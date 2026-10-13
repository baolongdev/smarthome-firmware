#pragma once
// Tasmota relays on the LAN, driven over the HTTP command API, in the
// "devices" FreeRTOS task (core 0) so slow/offline devices never delay
// MQTT or the UI.
//
//   poll:   GET http://<host>/cm?cmnd=Power0        -> {"POWER1":"ON",...}
//   toggle: GET http://<host>/cm?cmnd=Power<n>%20TOGGLE -> {"POWER<n>":"OFF"}
//
// State is pushed to the HMI with hmi::setDeviceState(); online/offline
// transitions and toggles are logged to the SD card.

#include "hmi.h"

namespace tasmota {

// Device list: from /config.json if it has "tasmota", else config.h.
void start(const hmi::DeviceInfo* list, uint8_t count);
void toggle(uint8_t device, uint8_t channel);  // any task, non-blocking
void print(Print& out);                        // console "devices"

}  // namespace tasmota
