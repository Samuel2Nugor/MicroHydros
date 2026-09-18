# Telemetry Simulator

A development tool that imitates the ESP32-S3 by publishing repeated raw sensor telemetry over MQTT, following `docs/data-contract.md`.

## Requirements

* Python 3
* The `paho-mqtt` package
* The MicroHydros Docker backend running (see `docker/README.md`), specifically Mosquitto on `localhost:1883`

## Setup

Create a virtual environment, then install the MQTT client library from `requirements.txt` (pins the exact version the team uses).

**Windows:** use the `py` launcher rather than bare `python`/`pip` — Windows can hijack those commands with a Microsoft Store install stub even when Python is already installed, and `pip.exe` isn't always on PATH.

```powershell
py -m venv .venv
.venv\Scripts\Activate.ps1
py -m pip install -r requirements.txt
```

**macOS:** `python`/`pip` without a `3` usually don't exist at all — use `python3` (or `pip3`).

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
```

## Run

From the repository root:

**Windows:**

```powershell
py src/simulator/telemetry_simulator.py
```

**macOS:**

```bash
python3 src/simulator/telemetry_simulator.py
```

The simulator connects to Mosquitto, then publishes one message every 30 seconds as device `simulator-01` to:

```text
microhydros/v1/devices/simulator-01/telemetry/raw
```

Each message includes `schema_version`, `device_id`, `boot_id`, an incrementing `sequence`, `uptime_ms`, all four required measurements (`internal_temperature_c`, `internal_humidity_percent`, `external_temperature_c`, `water_temperature_c`) with small randomized variation, and `sensor_status` for each sensor.

The script prints `Skickat sequence=<n>` for every message it sends. Stop it with `Ctrl+C`; it disconnects from the broker cleanly.

## Test

Open a second terminal and subscribe to the topic to confirm Mosquitto is receiving the messages:

```bash
docker exec -it microhydros-mosquitto mosquitto_sub -t "microhydros/v1/devices/+/telemetry/raw"
```

While the simulator is running, a new JSON message matching the data contract should appear in this terminal every 30 seconds.
