# Firmware

## Status

Draft — subject to team review and approval.

## Purpose

This directory contains the ESP-IDF firmware for the MicroHydros sensor node. It reads sensors every 5 seconds and publishes one raw telemetry message over MQTT per `docs/data-contract.md`.

Physical wiring, BOM and pinout live in `hardware/esp32s3/README.md`.

## Target

| Item | Value |
| ---- | ----- |
| MCU | ESP32-S3 DevKitC-1.1 |
| Framework | ESP-IDF `>=5.1.0` |
| Language | C |
| Publish interval | 5 seconds |

## Requirements

* [ESP-IDF v5.1+](https://docs.espressif.com/projects/esp-idf/en/v5.1.2/esp32s3/get-started/index.html) installed and exported (`. $HOME/esp/esp-idf/export.sh`)
* Python 3 (bundled with ESP-IDF)
* USB cable and either a working `/dev/ttyACM0` or `/dev/ttyUSB0`
* MicroHydros backend running (see `docker/README.md`) — the firmware publishes to it

Component dependencies (`onewire_bus`, `ds18b20`) are pulled automatically by the ESP-IDF component manager from `firmware/esp32s3/idf_component.yml`.

## First-time setup

```bash
cd firmware/esp32s3
idf.py set-target esp32s3          # required once per checkout
```

## Configuration

Open the menu:

```bash
idf.py menuconfig
```

Navigate to **MicroHydros Configuration** and set:

| Config | Description | Default |
| ------ | ----------- | ------- |
| `WIFI_SSID` | Wi-Fi SSID | `your_ssid` |
| `WIFI_PASSWORD` | Wi-Fi password | `your_password` |
| `MQTT_BROKER_URL` | Broker URL (see backend for port) | `mqtt://95.216.208.235:1883` |
| `DEVICE_ID` | Identifier used in topic and payload | `esp32s3-01` |
| `ONEWIRE_GPIO` | 1-Wire data pin | `4` |
| `DS18B20_WATER_ROM` | Pinned ROM (16 hex chars, empty = auto) | `""` |
| `DS18B20_EXTERNAL_ROM` | Pinned ROM (16 hex chars, empty = auto) | `""` |
| `I2C_SDA_GPIO` | I2C data pin | `8` |
| `I2C_SCL_GPIO` | I2C clock pin | `9` |
| `TELEMETRY_INTERVAL_SEC` | Publish interval in seconds | `5` |

Credentials are never committed. `sdkconfig` is `.gitignore`-d; only `sdkconfig.defaults` is versioned.

## Build

```bash
idf.py build
```

## Flash and monitor

```bash
idf.py -p /dev/ttyACM0 flash monitor      # adjust port if needed
```

Exit the monitor with `Ctrl-]`.

## DS18B20 role assignment (two-pass flash)

Both DS18B20 probes share one 1-Wire bus. The firmware needs to know which physical probe is the **water** probe and which is the **external air** probe. The role is set by ROM address.

### Pass 1 — discover ROMs

1. Leave `DS18B20_WATER_ROM` and `DS18B20_EXTERNAL_ROM` empty in `menuconfig`.
2. Flash and open the monitor. On boot the firmware logs:
   ```
   I sensor: DS18B20 discovered ROM: 28A1B2C3D4E5F601
   I sensor: DS18B20 discovered ROM: 28112233445566AA
   ```
3. Identify the water probe by immersing one probe in warm water while the monitor is open. Its next reading rises within 30 seconds; the corresponding `internal_temp/water/external` line in the log identifies which ROM is which.

### Pass 2 — pin the roles

1. Re-open `menuconfig` and set both ROM strings (16 hex chars, no `0x`, no separator).
2. Re-flash.
3. On next boot the log should say:
   ```
   I sensor: DS18B20 roles: water=ready external=ready
   ```
   without any `UNPINNED` warning.

## Expected serial output

Every telemetry cycle logs one sensor line and one MQTT line:

```
I sensor: internal_temp=23.62(ok) internal_hum=48.1(ok) water=20.75(ok) external=18.94(ok)
I mqtt: published (msg_id=42) seq=1
I mqtt: {"schema_version":1,"device_id":"esp32s3-01",...}
```

Status values follow the data contract:

| Status | Meaning |
| ------ | ------- |
| `ok` | Reading valid |
| `read_error` | Sensor detected but read failed (I2C error, CRC mismatch, 1-Wire fault) |
| `not_detected` | Sensor missing at boot |

Plausibility checks are the validator's job (`docker/node-red/flows/mqtt-validation.json`) — the firmware forwards raw readings.

## Runtime behavior

- **Non-blocking network** — Wi-Fi waits up to 30 s, MQTT waits up to 60 s. Both continue on timeout; ESP-IDF's client auto-reconnects.
- **Telemetry task** — the `esp_timer` callback only sends `xTaskNotifyGive`; sensor reads run in a dedicated FreeRTOS task. This prevents watchdog trips from the ~1.6 s DS18B20 conversion delay.
- **SHT31 CRC8** — every I2C read verifies both CRC bytes (polynomial `0x31`, init `0xFF`). CRC mismatch → `read_error`.
- **LWT (Last Will and Testament)** — configured before connect; broker publishes `offline` if the device drops.
- **Retained `online` status** — published on `MQTT_EVENT_CONNECTED`.

## Layout

```
firmware/esp32s3/
├── CMakeLists.txt          # project entry (idf.py hooks in here)
├── idf_component.yml       # component-manager deps (onewire_bus, ds18b20)
├── sdkconfig.defaults      # committed defaults
└── main/
    ├── CMakeLists.txt      # REQUIRES esp_wifi esp_event esp_netif nvs_flash
    │                       #          driver esp_timer json mqtt onewire_bus ds18b20
    ├── Kconfig.projbuild   # menuconfig options
    └── main.c              # entry point (app_main)
```

## Troubleshooting

| Symptom | Likely cause |
| ------- | ------------ |
| `SHT31 @0x44: NOT detected` | Wiring (SDA/SCL swapped, missing pull-up, bad 3V3), address mismatch |
| Both DS18B20 log `MISSING` | Missing 4.7 kΩ pull-up on GPIO4, wrong GPIO in menuconfig, dead probe |
| `UNPINNED` warning after Pass 2 | ROM string mistyped or one probe missing on bus |
| `not connected within 30s` (Wi-Fi) | SSID/password wrong or AP out of range — firmware still runs, will auto-reconnect |
| `not connected within 60s` (MQTT) | Broker URL wrong or backend not up — same, will auto-reconnect |
| No message on `telemetry/rejected` topic but Grafana is empty | Node-RED `MEASUREMENT_MAP` mismatch — check `docker/node-red/flows/mqtt-validation.json` |
