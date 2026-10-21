# ESP32-4848S040 Smart Home Panel Firmware

Firmware for the GUITION ESP32-4848S040 (ESP32-S3 N16R8, 4" 480×480 RGB LCD,
GT911 touch, 3 relays, TF card) running as a smart home control panel:

- LVGL 9 HMI: Overview gauge, Control (panel relays + Tasmota devices), Settings
  with sub-pages (Wi-Fi, network, system, SD card, sensor history, event log,
  file viewer with CSV tables and image preview), HCMUT splash and screensaver
- FreeRTOS architecture: `ui`, `app`, `net`, `devices`, `storage`, `console`
  tasks plus two LVGL draw threads (both cores)
- Wi-Fi scan / join with on-screen keyboard (open and saved networks), MQTT, NTP, ArduinoOTA
- Tasmota devices through the web gateway `https://smarthome.blong.id.vn/api/tasmota`
  (HTTPS keep-alive, 1 s polling; the panel only needs Internet)
- TF card + LittleFS: `config.json` (with a flash copy), event + sensor logs,
  sensor history (24 h / 7 d in PSRAM), firmware update with backup, screenshots

**Full documentation (Vietnamese): [docs/README.md](docs/README.md)**

| Doc | Topic |
|---|---|
| [01-phan-cung](docs/01-phan-cung.md) | hardware, verified GPIO map, TF card, Wi-Fi antenna notes |
| [02-build-nap](docs/02-build-nap.md) | required `platformio.ini`, the 8/16 MB flash header bug, serial, OTA |
| [03-kien-truc-rtos](docs/03-kien-truc-rtos.md) | tasks, queues, locking, watchdog, source map |
| [04-giao-dien-hmi](docs/04-giao-dien-hmi.md) | screens, states, palettes, fonts/icons, HMI API |
| [05-ket-noi](docs/05-ket-noi.md) | Wi-Fi manager, MQTT topics/payloads, Tasmota web gateway, NTP |
| [06-the-nho](docs/06-the-nho.md) | card layout, `config.json` schema, logs, SD firmware update, file viewer, PSRAM/LittleFS use |
| [07-go-loi](docs/07-go-loi.md) | serial commands, screenshots, reset reasons, troubleshooting |
| [08-nhat-ky](docs/08-nhat-ky.md) | change log: problems found and how they were fixed |

## Quick start

```bash
pio run -t upload --upload-port COM4       # use your CH340 port
pio device monitor -p COM4 -b 115200
python tools/screenshot.py COM4 shot.png   # current screen as PNG (close the monitor first)
```

Screenshots of every screen: [docs/images](docs/images) (gallery in [docs/README.md](docs/README.md)).

`board_upload.flash_size = 16MB` in `platformio.ini` is required: without it the
image header says 8 MB and the (silent) bootloader refuses to start the app.

## Configuration

- Compile-time defaults: `include/config.h` (Wi-Fi, MQTT, gauge metric, Tasmota
  web gateway + devices, relay names, backlight floor, screensaver delay).
- Runtime overrides: `/config.json` on the TF card (see docs/06-the-nho.md); a
  copy is kept in LittleFS for boots without a card.
- Wi-Fi joined from the panel, brightness, screensaver switch and colour palette are kept in NVS.

## MQTT (base `smarthome/device/esp32-4848s040`)

| Topic | Dir | Payload |
|---|---|---|
| `/state` | out, retained | device info + `relays[]` |
| `/availability` | out, retained (LWT) | `online` / `offline` |
| `/command` | in | `{"relay":0,"state":true}`, `{"relays":[true,false,true]}`, `{"brightness":0-255}` |
| `/sensor` | in | `{"primary":26.4,"metric1":62,"metric2":412}` or `{"error":true}` |

`/config` is subscribed but not handled yet; MQTT OTA and a web `/update` page are not implemented.

## License

MIT. Third-party: Be Vietnam Pro (OFL) and Lucide icons (ISC), see `assets/fonts/`.
