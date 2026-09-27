# ESP32-4848S040 Firmware

Firmware for ESP32-S3 based 4.8" 480x480 RGB display panel (ESP32-4848S040) running as a smart home control panel with 4-channel relay control.

## Hardware Specifications

| Component | Specification |
|-----------|---------------|
| **MCU** | ESP32-S3 (Xtensa LX7 dual-core, 240MHz) |
| **Flash** | 16MB QSPI |
| **PSRAM** | 8MB Octal PSRAM |
| **Display** | 4.8" 480x480 RGB (parallel 16-bit) |
| **Touch** | GT911 I2C capacitive touch |
| **Relays** | 4-channel (GPIO 1, 2, 42, 41) |
| **Buttons** | 4x tactile buttons (GPIO 0, 9, 10, 11) |
| **Backlight** | PWM controlled (GPIO 45) |

## Pin Mapping

### RGB Display (Parallel 16-bit)
| Signal | GPIO |
|--------|------|
| D0-D15 | 39-24 |
| WR (PCLK) | 40 |
| RD | 41 |
| DC | 44 |
| RST | 46 |
| BL (PWM) | 45 |

### Touch (I2C - GT911)
| Signal | GPIO |
|--------|------|
| SDA | 47 |
| SCL | 21 |

### Relays (Active HIGH)
| Channel | GPIO |
|---------|------|
| Relay 1 | 1 |
| Relay 2 | 2 |
| Relay 3 | 42 |
| Relay 4 | 41 |

### Buttons (Active LOW with pullup)
| Button | GPIO |
|--------|------|
| Button 1 | 0 |
| Button 2 | 9 |
| Button 3 | 10 |
| Button 4 | 11 |

## Software Stack

- **Framework**: Arduino (PlatformIO)
- **Display**: LovyanGFX (RGB panel driver)
- **MQTT**: PubSubClient
- **JSON**: ArduinoJson v7
- **Web Server**: ESPAsyncWebServer (for OTA)
- **Buttons**: OneButton library

## Building & Flashing

### Prerequisites
- PlatformIO CLI or VS Code with PlatformIO extension
- ESP32-S3 board support installed

### Commands
```bash
# Build
pio run

# Flash via USB
pio run -t upload

# Flash via OTA (configure upload_port in platformio.ini)
pio run -t upload -e esp32-4848s040-ota

# Monitor serial
pio device monitor

# Clean build
pio run -t clean
```

### First Flash
1. Connect ESP32-4848S040 via USB-C
2. Put in boot mode (hold BOOT, press RST, release BOOT)
3. Run `pio run -t upload`

## Configuration

Edit `include/config.h` with your settings:
- WiFi credentials
- MQTT broker address
- Device names

## MQTT Integration

### Topics
| Topic | Direction | Description |
|-------|-----------|-------------|
| `smarthome/device/esp32-4848s040/state` | Pub | Device state (JSON) |
| `smarthome/device/esp32-4848s040/command` | Sub | Commands (JSON) |
| `smarthome/device/esp32-4848s040/availability` | Pub | online/offline |
| `smarthome/device/esp32-4848s040/config` | Sub | Configuration updates |
| `smarthome/device/esp32-4848s040/ota` | Sub | OTA trigger |

### Command Format
```json
// Toggle single relay
{"relay": 0, "state": true}

// Set multiple relays
{"relays": [true, false, true, false]}

// Set display brightness (0-255)
{"brightness": 200}
```

### State Format
```json
{
  "device": "ESP32-4848S040 Panel",
  "type": "panel",
  "fw_version": "1.0.0",
  "uptime": 3600,
  "free_heap": 123456,
  "wifi_rssi": -45,
  "ip": "192.168.1.100",
  "relays": [
    {"index": 0, "state": true, "last_change": 120},
    {"index": 1, "state": false, "last_change": 3600}
  ]
}
```

## Home Assistant Integration

Add to `configuration.yaml`:
```yaml
mqtt:
  switch:
    - name: "Panel Relay 1"
      unique_id: "esp32_4848s040_relay_1"
      state_topic: "smarthome/device/esp32-4848s040/state"
      command_topic: "smarthome/device/esp32-4848s040/command"
      value_template: "{{ value_json.relays[0].state }}"
      payload_on: '{"relay": 0, "state": true}'
      payload_off: '{"relay": 0, "state": false}'
      availability_topic: "smarthome/device/esp32-4848s040/availability"
      # Repeat for relays 2-4 with index 1,2,3
```

## OTA Updates

### Via PlatformIO
```bash
pio run -t upload -e esp32-4848s040-ota
```

### Via Web Interface
1. Access `http://<device-ip>/update`
2. Upload firmware.bin

### Via MQTT
Publish to `smarthome/device/esp32-4848s040/ota`:
```json
{"url": "http://192.168.1.100/firmware.bin"}
```

## Troubleshooting

### Display not working
- Check RGB wiring (16 data lines + control)
- Verify PSRAM is enabled in platformio.ini
- Check power supply (display needs ~500mA)

### WiFi/MQTT issues
- Verify credentials in config.h
- Check serial monitor for connection logs
- Ensure MQTT broker allows anonymous or correct credentials

### Touch not working
- GT911 uses I2C address 0x5D or 0x14
- Check I2C wiring (SDA=47, SCL=21)
- Add pull-up resistors if needed (4.7kΩ)

## License

MIT License - See LICENSE file for details.