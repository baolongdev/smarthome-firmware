#pragma once

#ifndef CONFIG_H
#define CONFIG_H

// ===== Wi-Fi Configuration =====
// Known networks are tried in order; the panel also remembers the last one
// joined from its Wi-Fi screen (NVS).  /config.json on the SD card can list more.
#define WIFI_SSID "RD-SEAI_2.4G"
#define WIFI_PASSWORD ""
#define WIFI_HOSTNAME "esp32-4848s040"

// ===== ESP-NOW (panel = slave, receives relay commands from master) =====
#define ESPNOW_ENABLED true
#define ESPNOW_CHANNEL 1  // used when Wi-Fi is not connected; STA-connected ESP-NOW follows Wi-Fi channel
#define ESPNOW_PMK "00112233445566778899AABBCCDDEEFF"
#define ESPNOW_LMK "00112233445566778899AABBCCDDEEFF"
// Master MAC that may control this panel. Set to your real master MAC.
#define ESPNOW_MASTER_MAC "AA:BB:CC:DD:EE:FF"

// ===== Display Configuration (ESP32-4848S040) =====
#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 480
#define DISPLAY_ROTATION 0
#define DISPLAY_BRIGHTNESS 200
#define DISPLAY_BACKLIGHT_PIN 38

// ===== HMI (LVGL) =====
#define HMI_ROOM_NAME "Phòng khách"            // header title (UTF-8)
#define HMI_DIM_AFTER_MS 120000                // screensaver + dim after 2 min idle (toggle in Settings)
#define HMI_DIM_BRIGHTNESS_PCT 0               // auto-dim level on the slider scale (0 = floor)
// Slider 0..100 % maps linearly onto LEDC duty HMI_BACKLIGHT_MIN_DUTY..255, so
// "0 %" is still half brightness: the panel never goes dark from the slider.
#define HMI_BACKLIGHT_MIN_DUTY 128
#define HMI_TZ_OFFSET_SECONDS (7 * 3600)

// Primary gauge + two metric cards.  Labels/units/scale are data: change them
// for another sensor without touching the UI code.
#define HMI_PRIMARY_LABEL "NHIỆT ĐỘ PHÒNG"
#define HMI_PRIMARY_UNIT "°C"
#define HMI_PRIMARY_MIN 16.0f
#define HMI_PRIMARY_MAX 36.0f
#define HMI_PRIMARY_WARN 30.0f
#define HMI_METRIC1_LABEL "Độ ẩm"
#define HMI_METRIC1_UNIT "%"
#define HMI_METRIC2_LABEL "Công suất"
#define HMI_METRIC2_UNIT "W"

// No sensor wired: simulate values (ESP-NOW-only build can still show demo data).
#define HMI_SIMULATE_SENSORS 1
#define HMI_SENSOR_STALE_MS 60000

// Optional physical buttons (the 4848S040 has none free: GPIO 9-12 drive the LCD).
// Define all three to enable focus navigation without touch.
// #define HMI_BTN_PREV_PIN x
// #define HMI_BTN_NEXT_PIN x
// #define HMI_BTN_OK_PIN   x

// Tasmota/HTTP devices disabled in ESP-NOW-only build.
#define TASMOTA_API_URL ""
#define TASMOTA_1_NAME "Relay 1"
#define TASMOTA_1_KEY "r1"
#define TASMOTA_1_CHANNELS 1
#define TASMOTA_2_NAME "Relay 2"
#define TASMOTA_2_KEY "r2"
#define TASMOTA_2_CHANNELS 1
#define TASMOTA_POLL_MS 5000

// UTF-8 relay names (Control screen).
#define RELAY_NAME_1 "Phòng khách"
#define RELAY_NAME_2 "Điều hòa"
#define RELAY_NAME_3 "Ổ cắm"

// ===== GPIO Pin Definitions =====
// RGB Interface (parallel 16-bit)
#define PIN_DISP_D0 4   // B0
#define PIN_DISP_D1 5   // B1
#define PIN_DISP_D2 6   // B2
#define PIN_DISP_D3 7   // B3
#define PIN_DISP_D4 15  // B4
#define PIN_DISP_D5 8   // G0
#define PIN_DISP_D6 20  // G1
#define PIN_DISP_D7 3   // G2
#define PIN_DISP_D8 46  // G3
#define PIN_DISP_D9 9   // G4
#define PIN_DISP_D10 10 // G5
#define PIN_DISP_D11 11 // R0
#define PIN_DISP_D12 12 // R1
#define PIN_DISP_D13 13 // R2
#define PIN_DISP_D14 14 // R3
#define PIN_DISP_D15 0  // R4
#define PIN_DISP_DE 18
#define PIN_DISP_VSYNC 17
#define PIN_DISP_HSYNC 16
#define PIN_DISP_PCLK 21
#define PIN_DISP_CS 39
#define PIN_DISP_SCLK 48
#define PIN_DISP_MOSI 47
#define PIN_DISP_RST -1
#define PIN_DISP_BL 38

// I2C for touch (GT911)
#define PIN_TOUCH_SDA 19
#define PIN_TOUCH_SCL 45
#define PIN_TOUCH_INT -1
#define PIN_TOUCH_RST -1

// ===== Device Configuration =====
#define DEVICE_NAME "Smart Home Panel"
#define DEVICE_TYPE "panel"
#define FIRMWARE_VERSION "1.1.0-ui"
#define HARDWARE_VERSION "1.0"

// ===== Relay/Output Pins (4-channel) =====
#define RELAY_COUNT 3
#define RELAY_PIN_1 40
#define RELAY_PIN_2 2
#define RELAY_PIN_3 1

// ===== Input Pins (buttons) =====
// Note: GPIO 0 is a boot strapping pin - cannot use as input!
#define BUTTON_PIN_1 9
#define BUTTON_PIN_2 10
#define BUTTON_PIN_3 11
#define BUTTON_PIN_4 12

// ===== OTA Configuration =====
#define OTA_ENABLED true
#define OTA_PORT 3232
#define OTA_PASSWORD "admin"

// ===== System =====
#define SERIAL_BAUDRATE 115200
#define BUTTONS_ENABLED 0

#define WATCHDOG_TIMEOUT 30
#define HEAP_MONITOR_INTERVAL 30000

#endif // CONFIG_H
