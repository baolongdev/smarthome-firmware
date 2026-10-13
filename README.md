# ESP32-4848S040 Smart Home Panel Firmware

Firmware for the GUITION ESP32-4848S040 (ESP32-S3 N16R8, 4" 480×480 RGB LCD,
GT911 touch, 3 relays, TF card) running as a smart home control panel:

- LVGL 9 HMI: Overview gauge, Control (panel relays + Tasmota devices), Settings, Wi-Fi
- FreeRTOS architecture: `ui`, `app`, `net`, `devices`, `storage`, `console` tasks
- Wi-Fi scan / join with on-screen keyboard, MQTT, NTP, ArduinoOTA
- Tasmota devices on the LAN via their HTTP API
- TF card: `config.json`, event + sensor logs, firmware update, screenshots

**Full documentation (Vietnamese): [docs/README.md](docs/README.md)**

| Doc | Topic |
|---|---|
| [01-phan-cung](docs/01-phan-cung.md) | hardware, verified GPIO map, TF card, Wi-Fi antenna notes |
| [02-build-nap](docs/02-build-nap.md) | required `platformio.ini`, the 8/16 MB flash header bug, serial, OTA |
| [03-kien-truc-rtos](docs/03-kien-truc-rtos.md) | tasks, queues, locking, watchdog, source map |
| [04-giao-dien-hmi](docs/04-giao-dien-hmi.md) | screens, states, palettes, fonts/icons, HMI API |
| [05-ket-noi](docs/05-ket-noi.md) | Wi-Fi manager, MQTT topics/payloads, Tasmota, NTP |
| [06-the-nho](docs/06-the-nho.md) | card layout, `config.json` schema, logs, SD firmware update |
| [07-go-loi](docs/07-go-loi.md) | serial commands, screenshots, reset reasons, troubleshooting |
| [08-nhat-ky](docs/08-nhat-ky.md) | change log: problems found and how they were fixed |

## Quick start

```bash
pio run -t upload --upload-port COM18      # use your CH340 port
pio device monitor -p COM18 -b 115200
python tools/screenshot.py COM18 shot.png  # current screen as PNG (close the monitor first)
```

`board_upload.flash_size = 16MB` in `platformio.ini` is required: without it the
image header says 8 MB and the (silent) bootloader refuses to start the app.

## Configuration

- Compile-time defaults: `include/config.h` (Wi-Fi, MQTT, gauge metric, Tasmota devices, relay names).
- Runtime overrides: `/config.json` on the TF card (see docs/06-the-nho.md).
- Wi-Fi joined from the panel, brightness, auto-dim and colour palette are kept in NVS.

## MQTT (base `smarthome/device/esp32-4848s040`)

| Topic | Dir | Payload |
|---|---|---|
| `/state` | out, retained | device info + `relays[]` |
| `/availability` | out, retained (LWT) | `online` / `offline` |
| `/command` | in | `{"relay":0,"state":true}`, `{"relays":[true,false,true]}`, `{"brightness":0-255}` |
| `/sensor` | in | `{"primary":26.4,"metric1":62,"metric2":412}` or `{"error":true}` |

`/config` is subscribed but not handled yet; MQTT OTA and a web `/update` page are not implemented.

## License

MIT License - See LICENSE file for details.
