# Telemetry Simulator

A development tool that imitates the ESP32-S3 by publishing repeated raw sensor telemetry over MQTT, following `docs/data-contract.md`.

## Requirements

* Python 3
* The `paho-mqtt` package
* The MicroHydros Docker backend running (see `docker/README.md`), specifically Mosquitto on `localhost:1883`

## Setup

Install the MQTT client library:

```bash
pip install paho-mqtt
```

## Run

From the repository root:

```bash
python src/simulator/telemetry_simulator.py
```

The simulator connects to Mosquitto, then publishes one message every 5 seconds as device `simulator-01` to:

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

While the simulator is running, a new JSON message matching the data contract should appear in this terminal every 5 seconds.
