# Node-RED Flows

This directory contains exported Node-RED flows for the MicroHydros telemetry pipeline.

## Flows

### MQTT Validation Flow

**File:** `mqtt-validation.json`

Validates raw telemetry messages against the data contract in `docs/data-contract.md`. Performs message-level and measurement-level validation, rejecting invalid data while routing valid measurements downstream.

### InfluxDB Storage Flow

**File:** `influxdb-storage.json`

Stores validated measurements in InfluxDB. Receives validated measurements from the MQTT validation flow and writes them to a time-series database for historical analysis and Grafana visualization.

## Import flows

1. Open Node-RED at `http://localhost:1880`.
2. Open the menu in the upper-right corner.
3. Select **Import**.
4. Select `docker/node-red/flows/mqtt-validation.json` (or `influxdb-storage.json`).
5. Import the flow into a new tab.
6. Click **Deploy**.

For the complete telemetry pipeline, import both flows.

## MQTT Validation Flow — detailed documentation

### Purpose

Validates raw telemetry from the ESP32-S3 or simulator against the data contract. Performs two-stage validation:

- **Message scope**: Checks JSON structure, device_id, schema_version, boot_id, sequence, uptime_ms
- **Measurement scope**: For each measurement, validates sensor status, value type, and plausibility ranges

### Flow structure

```text
Raw telemetry (mqtt in)
    → Validate telemetry (Function node)
    ├→ Publish validated (MQTT output)
    └→ Publish rejected (MQTT output) + Rejected debug
```

### Testing

#### Test 1: Valid payload

All four measurements validate and route to individual `validated/` topics:

```bash
docker exec microhydros-mosquitto mosquitto_pub -h localhost \
  -t microhydros/v1/devices/simulator-01/telemetry/raw \
  -q 1 \
  -m '{"schema_version":1,"device_id":"simulator-01","boot_id":"test-boot-01","sequence":1,"uptime_ms":30000,"measurements":{"internal_temperature_c":23.5,"internal_humidity_percent":55.2,"external_temperature_c":18.4,"water_temperature_c":20.7},"sensor_status":{"internal_sht31":"ok","external_ds18b20":"ok","water_ds18b20":"ok"}}'
```

**Expected**: Nothing in debug panel; four messages to `validated/` topics.

#### Test 2: Sensor failure — per-measurement isolation

One sensor fails; other measurements still validate:

```bash
docker exec microhydros-mosquitto mosquitto_pub -h localhost \
  -t microhydros/v1/devices/simulator-01/telemetry/raw \
  -q 1 \
  -m '{"schema_version":1,"device_id":"simulator-01","boot_id":"test-boot-01","sequence":2,"uptime_ms":60000,"measurements":{"internal_temperature_c":23.5,"internal_humidity_percent":55.2,"external_temperature_c":null,"water_temperature_c":20.7},"sensor_status":{"internal_sht31":"ok","external_ds18b20":"read_error","water_ds18b20":"ok"}}'
```

**Expected**: Three measurements to `validated/` topics; one rejection in debug with `reason_code: "sensor_read_error"`.

#### Test 3: Message rejection — whole-message failure

Device_id in payload does not match topic:

```bash
docker exec microhydros-mosquitto mosquitto_pub -h localhost \
  -t microhydros/v1/devices/simulator-01/telemetry/raw \
  -q 1 \
  -m '{"schema_version":1,"device_id":"esp32s3-01","boot_id":"test-boot-01","sequence":3,"uptime_ms":90000,"measurements":{"internal_temperature_c":23.5,"internal_humidity_percent":55.2,"external_temperature_c":18.4,"water_temperature_c":20.7},"sensor_status":{"internal_sht31":"ok","external_ds18b20":"ok","water_ds18b20":"ok"}}'
```

**Expected**: No messages to `validated/` topics; one rejection in debug with `reason_code: "device_id_mismatch"`.

### Validation rules

Message scope (all required):

- `device_id` is a non-empty string and matches MQTT topic
- `schema_version` is 1
- `boot_id` is a non-empty string
- `sequence` is a non-negative integer
- `uptime_ms` is a non-negative integer
- `measurements` and `sensor_status` are objects

Measurement scope (per-measurement):

- Measurement field exists
- Sensor status field exists and is one of: `ok`, `read_error`, `not_detected`, `invalid_value`
- If status is `ok`: value is not null, is a finite number, and falls within plausible range
- If status is not `ok`: measurement is rejected

Plausible ranges:

- `internal_temperature_c`: [-10.0, 60.0]
- `internal_humidity_percent`: [0.0, 100.0]
- `external_temperature_c`: [-40.0, 60.0]
- `water_temperature_c`: [0.0, 50.0]

Deduplication: Messages with identical `(device_id, boot_id, sequence)` are silently dropped if already processed.

## InfluxDB Storage Flow — detailed documentation

### Purpose

Stores validated measurements from the MQTT validation flow in InfluxDB for historical analysis.

### Flow structure

```text
Test injection (development only)
    → Create four test measurements
    → Build InfluxDB write
    → InfluxDB HTTP write
    → Debug response
```

Also includes MQTT subscription to `microhydros/v1/devices/+/telemetry/validated/+` for production use.

### Storage schema

Measurement name: `sensor_reading`

Tags (indexed for filtering):

- `device_id` — source device
- `measurement_type` — measurement name (e.g., "water_temperature")
- `sensor_id` — physical sensor identifier
- `unit` — measurement unit

Fields (stored values):

- `value` — numeric measurement
- `schema_version` — contract version
- `sequence` — device message sequence number
- `uptime_ms` — device uptime when measured
- `boot_id` — device boot session identifier

Timestamp: UTC time assigned by Node-RED at validation.

### Testing

Press the test Inject node to send example measurements. The debug panel should show four HTTP 204 responses (successful writes).

Query stored values:

```bash
docker compose exec influxdb sh -c '
influx query \
"from(bucket: \"${DOCKER_INFLUXDB_INIT_BUCKET}\")
  |> range(start: -15m)
  |> filter(fn: (r) => r._measurement == \"sensor_reading\")
  |> filter(fn: (r) => r._field == \"value\")
  |> keep(columns: [\"_time\", \"_value\", \"device_id\", \"measurement_type\", \"sensor_id\", \"unit\"])" \
--org "$DOCKER_INFLUXDB_INIT_ORG" \
--token "$DOCKER_INFLUXDB_INIT_ADMIN_TOKEN"
'
```

## Integration

For the complete telemetry pipeline:

1. Import both flows into Node-RED
2. Deploy both
3. The validation flow publishes to `microhydros/v1/devices/+/telemetry/validated/+`
4. The storage flow subscribes to that topic and writes to InfluxDB
5. Rejected messages (published to `.../telemetry/rejected`) are only seen in the validation flow's debug panel

## Limitations

- Deduplication uses in-memory flow context and does not persist across Node-RED restarts
- Timestamps assigned by Node-RED; incorrect system clock affects all timestamps
- Rejected messages are not retained; lost if no subscribers connected when published
- Storage flow uses InfluxDB admin token; should use scoped write-only token in production