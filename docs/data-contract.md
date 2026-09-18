# Data Contract

## Status

Draft — subject to team review and approval.

## Purpose

This document defines the MQTT topics and JSON message formats used by the MicroHydros system.

The contract allows the ESP32-S3, Mosquitto, Node-RED, InfluxDB and future services to be developed independently while using the same field names, data types and units.

Messages that do not follow this contract must not be published to validated telemetry topics.

## MQTT topics

The topic structure includes a contract version and device identifier so that additional devices can be supported later.

```text
microhydros/v1/devices/{device_id}/telemetry/raw
microhydros/v1/devices/{device_id}/telemetry/validated/{measurement}
microhydros/v1/devices/{device_id}/telemetry/rejected
microhydros/v1/devices/{device_id}/status
```

For the first prototype, the device identifier is:

```text
esp32s3-01
```

The raw telemetry, rejected and status topics are:

```text
microhydros/v1/devices/esp32s3-01/telemetry/raw
microhydros/v1/devices/esp32s3-01/telemetry/rejected
microhydros/v1/devices/esp32s3-01/status
```

Each successfully validated measurement is published independently:

```text
microhydros/v1/devices/esp32s3-01/telemetry/validated/internal_temperature
microhydros/v1/devices/esp32s3-01/telemetry/validated/internal_humidity
microhydros/v1/devices/esp32s3-01/telemetry/validated/external_temperature
microhydros/v1/devices/esp32s3-01/telemetry/validated/water_temperature
```

## Topic responsibilities

| Topic suffix | Publisher | Subscriber | Purpose |
|---|---|---|---|
| `telemetry/raw` | ESP32-S3 or telemetry simulator | Node-RED | Carries one combined message containing sensor readings and statuses |
| `telemetry/validated/{measurement}` | Node-RED | Node-RED storage flow and future services | Carries one measurement that passed validation |
| `telemetry/rejected` | Node-RED | Node-RED diagnostic flow | Reports messages or measurements that failed validation |
| `status` | ESP32-S3 or Mosquitto LWT | Node-RED | Reports whether the device is online or offline |

After validation, Node-RED writes valid measurements to InfluxDB for historical storage.

Node-RED subscribes to raw telemetry from every compatible device using:

```text
microhydros/v1/devices/+/telemetry/raw
```

Node-RED and future services can subscribe to every validated measurement using:

```text
microhydros/v1/devices/+/telemetry/validated/+
```

The first `+` wildcard represents one device identifier. The second represents one measurement name.

## Development and physical device identifiers

The telemetry simulator and physical ESP32-S3 use the same MQTT topic structure and payload contract. They are distinguished by their device identifiers.

| Device | Device identifier |
|---|---|
| Telemetry simulator | `simulator-01` |
| Physical ESP32-S3 | `esp32s3-01` |

Example development topic:

```text
microhydros/v1/devices/simulator-01/telemetry/raw
```

Example physical-device topic:

```text
microhydros/v1/devices/esp32s3-01/telemetry/raw
```

The `device_id` inside the JSON payload must match the device identifier in the MQTT topic.

## Raw telemetry contract

The ESP32-S3 publishes one combined raw telemetry message every 5 seconds.

Each measurement is later validated independently by Node-RED. A failed measurement must not prevent other valid measurements from being published.

**Topic:**

```text
microhydros/v1/devices/{device_id}/telemetry/raw
```

**MQTT settings:**

| Property | Value |
| -------- | ----- |
| QoS | `1` |
| Retained | `false` |
| Payload format | UTF-8 JSON |
| Publishing interval | 5 seconds |

### Example valid raw payload

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "boot_id": "a3f82c10",
  "sequence": 42,
  "uptime_ms": 185430,
  "measurements": {
    "internal_temperature_c": 23.6,
    "internal_humidity_percent": 61.4,
    "external_temperature_c": 18.9,
    "water_temperature_c": 20.7
  },
  "sensor_status": {
    "internal_sht31": "ok",
    "external_ds18b20": "ok",
    "water_ds18b20": "ok"
  }
}
```

### Field definitions Raw payload

| Field | Type | Required | Description |
| ----- | ---- | -------- | ----------- |
| `schema_version` | Integer | Yes | Version of this data contract |
| `device_id` | String | Yes | Identifier of the publishing device |
| `boot_id` | String | Yes | Identifier generated when the ESP32-S3 starts |
| `sequence` | Integer | Yes | Message number during the current boot session |
| `uptime_ms` | Integer | Yes | Milliseconds since the ESP32-S3 started |
| `measurements` | Object | Yes | Collection of sensor measurements |
| `sensor_status` | Object | Yes | Current status of each physical sensor |

### Measurement fields

| Field | Type | Unit | Required |
| ----- | ---- | ---- | -------- |
| `internal_temperature_c` | Number or `null` | Degrees Celsius | Yes |
| `internal_humidity_percent` | Number or `null` | Percent relative humidity | Yes |
| `external_temperature_c` | Number or `null` | Degrees Celsius | Yes |
| `water_temperature_c` | Number or `null` | Degrees Celsius | Yes |

All four measurement fields must be present. A failed measurement uses `null`; the field must not be omitted.

### Plausibility ranges

These ranges determine whether a sensor value is technically plausible. They are deliberately broader than the desired growing-condition ranges and must not be used as alarm thresholds.

| Measurement field | Minimum | Maximum |
| ----------------- | ------- | ------- |
| `internal_temperature_c` | `-10.0` | `60.0` |
| `internal_humidity_percent` | `0.0` | `100.0` |
| `external_temperature_c` | `-40.0` | `60.0` |
| `water_temperature_c` | `0.0` | `50.0` |

A numeric value outside its configured range is rejected with the reason code `out_of_plausible_range`.

These initial ranges may be revised after the sensors have been physically tested and calibrated.

### Sensor-status values

| Value | Meaning |
| ----- | ------- |
| `ok` | The sensor returned a usable reading |
| `read_error` | The sensor was detected but the reading failed |
| `not_detected` | The sensor could not be detected |
| `invalid_value` | The sensor returned an unusable value |

If a sensor status is not `ok`, its affected measurement value must be `null`.

`NaN`, positive infinity and negative infinity are not valid JSON values and must never be published.

### Identifier rules

- `device_id` must match the device identifier in the MQTT topic.
- `boot_id` is generated once whenever the ESP32-S3 starts.
- `sequence` starts at `0` and increases by one for every raw message.
- `sequence` may reset only when the device restarts.
- `uptime_ms` must be a non-negative integer.

## Validated measurement contract

Node-RED validates every measurement independently. Each valid measurement is published as a separate MQTT message.

A failed measurement does not prevent other valid measurements from being published.

**Topic:**

```text
microhydros/v1/devices/{device_id}/telemetry/validated/{measurement}
```

**MQTT settings:**

| Property | Value |
| -------- | ----- |
| QoS | `1` |
| Retained | `false` |
| Payload format | UTF-8 JSON |

### Measurement mapping

| Measurement | Source sensor | Unit |
| ----------- | ------------- | ---- |
| `internal_temperature` | `internal_sht31` | `celsius` |
| `internal_humidity` | `internal_sht31` | `percent_rh` |
| `external_temperature` | `external_ds18b20` | `celsius` |
| `water_temperature` | `water_ds18b20` | `celsius` |

### Example validated measurement

**Topic:**

```text
microhydros/v1/devices/esp32s3-01/telemetry/validated/water_temperature
```

**Payload:**

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "boot_id": "a3f82c10",
  "sequence": 42,
  "uptime_ms": 185430,
  "timestamp": "2026-09-08T10:15:30Z",
  "measurement": "water_temperature",
  "value": 20.7,
  "unit": "celsius",
  "sensor_id": "water_ds18b20"
}
```

### Field definitions Valid measurement

| Field | Type | Required | Description |
| ----- | ---- | -------- | ----------- |
| `schema_version` | Integer | Yes | Supported contract version |
| `device_id` | String | Yes | Identifier of the source device |
| `boot_id` | String | Yes | Identifier of the current device boot session |
| `sequence` | Integer | Yes | Sequence number from the original raw message |
| `uptime_ms` | Integer | Yes | Device uptime from the original raw message |
| `timestamp` | String | Yes | UTC reception time assigned by Node-RED |
| `measurement` | String | Yes | Name of the validated measurement |
| `value` | Number | Yes | Validated measurement value |
| `unit` | String | Yes | Unit associated with the measurement |
| `sensor_id` | String | Yes | Sensor that produced the measurement |

### Publication rules

Node-RED publishes a validated measurement only when:

- The corresponding raw measurement field exists.
- Its value is a finite number.
- The relevant sensor status is `ok`.
- The value passes the configured plausibility checks.
- The device ID matches the MQTT topic.
- The schema version is supported.
- The measurement name in the payload matches the final topic segment.

Node-RED assigns one timestamp to the raw message before splitting it. All valid measurements originating from the same raw message therefore receive the same timestamp, `boot_id` and `sequence`.

Invalid values, `null`, `NaN` and infinity must never appear on a validated topic.

## Rejected telemetry contract

Node-RED publishes validation failures to the rejected topic.

**Topic:**

```text
microhydros/v1/devices/{device_id}/telemetry/rejected
```

**MQTT settings:**

| Property | Value |
| -------- | ----- |
| QoS | `1` |
| Retained | `false` |
| Payload format | UTF-8 JSON |

There are two rejection scopes:

- `message` — the complete raw message cannot be processed.
- `measurement` — one measurement failed, but other valid measurements may continue.

## Message-level rejection

The complete raw message is rejected when its common metadata or JSON structure cannot be trusted.

Examples include:

- Empty MQTT payload
- Invalid JSON
- Missing or invalid `device_id`
- Device ID does not match the MQTT topic
- Unsupported `schema_version`
- Missing or invalid `boot_id`
- Missing or invalid `sequence`
- Missing or invalid `uptime_ms`

### Example message-level rejection

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "timestamp": "2026-09-08T10:15:30Z",
  "rejection_scope": "message",
  "reason_code": "invalid_json",
  "description": "The raw MQTT payload could not be parsed as JSON"
}
```

When a message-level rejection occurs, none of its measurements may be published to validated topics.

## Measurement-level rejection

A single measurement is rejected when its value or corresponding sensor status is invalid.

Other valid measurements from the same raw message may still be published.

### Example measurement-level rejection

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "boot_id": "a3f82c10",
  "sequence": 43,
  "timestamp": "2026-09-08T10:16:00Z",
  "rejection_scope": "measurement",
  "measurement": "external_temperature",
  "sensor_id": "external_ds18b20",
  "received_value": null,
  "reason_code": "sensor_not_detected",
  "description": "The external SHT31 sensor could not be detected"
}
```

### Rejected-field definitions

| Field | Type | Required | Description |
| ----- | ---- | -------- | ----------- |
| `schema_version` | Integer | Yes | Version of the rejected-message contract |
| `device_id` | String | Yes | Device identifier extracted from the MQTT topic |
| `boot_id` | String | Measurement rejection only | Source device boot session |
| `sequence` | Integer | Measurement rejection only | Sequence number of the raw message |
| `timestamp` | String | Yes | UTC rejection time assigned by Node-RED |
| `rejection_scope` | String | Yes | Either `message` or `measurement` |
| `measurement` | String | Measurement rejection only | Measurement that failed validation |
| `sensor_id` | String | Measurement rejection only | Sensor associated with the failure |
| `received_value` | Any JSON type | Measurement rejection only | Value received by Node-RED |
| `reason_code` | String | Yes | Machine-readable reason for rejection |
| `description` | String | Yes | Human-readable explanation |

### Reason codes

| Reason code | Scope | Meaning |
| ----------- | ----- | ------- |
| `empty_payload` | Message | The MQTT payload was empty |
| `invalid_json` | Message | The payload was not valid JSON |
| `unsupported_schema` | Message | The schema version is not supported |
| `device_id_mismatch` | Message | Payload and topic device identifiers differ |
| `missing_metadata` | Message | Required common metadata is missing |
| `invalid_metadata` | Message | Required common metadata has an invalid type or value |
| `missing_measurement` | Measurement | A required measurement field is missing |
| `missing_sensor_status` | Measurement | The required sensor-status field is missing |
| `invalid_sensor_status` | Measurement | The sensor status is not one of the supported values |
| `invalid_type` | Measurement | The measurement is not numeric |
| `sensor_read_error` | Measurement | The sensor reading failed |
| `sensor_not_detected` | Measurement | The sensor could not be detected |
| `invalid_value` | Measurement | The sensor returned an unusable value |
| `out_of_plausible_range` | Measurement | The value is outside the configured plausibility range |

Rejected messages are diagnostic information. They must never be treated as validated telemetry or written into normal InfluxDB measurement series.

## Device-status contract

The device-status topic reports whether an ESP32-S3 is connected to the MQTT broker.

**Topic:**

```text
microhydros/v1/devices/{device_id}/status
```

**MQTT settings:**

| Property | Value |
| -------- | ----- |
| QoS | `1` |
| Retained | `true` |
| Payload format | UTF-8 JSON |

Node-RED can subscribe to the status of every device using:

```text
microhydros/v1/devices/+/status
```

### Online status

After successfully connecting to Mosquitto, the ESP32-S3 publishes:

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "boot_id": "a3f82c10",
  "status": "online"
}
```

### Offline status

Before connecting, the ESP32-S3 configures the following MQTT Last Will and Testament message:

```json
{
  "schema_version": 1,
  "device_id": "esp32s3-01",
  "boot_id": "a3f82c10",
  "status": "offline"
}
```

If the ESP32-S3 unexpectedly loses power, Wi-Fi or its MQTT connection, Mosquitto publishes the offline message.

For a planned disconnection, the ESP32-S3 should publish the offline message before disconnecting.

### Status-field definitions

| Field | Type | Required | Description |
| ----- | ---- | -------- | ----------- |
| `schema_version` | Integer | Yes | Version of the status contract |
| `device_id` | String | Yes | Identifier of the device |
| `boot_id` | String | Yes | Identifier of the current boot session |
| `status` | String | Yes | Either `online` or `offline` |

### Status rules

- The device ID must match the identifier in the MQTT topic.
- The status message must be retained.
- The ESP32-S3 must configure its Last Will before connecting to Mosquitto.
- The ESP32-S3 publishes `online` only after the MQTT connection succeeds.
- Each new boot generates a new `boot_id`.
- Telemetry must not be retained, even though device status is retained.
- Status messages must not contain credentials or other secrets.

Node-RED assigns the reception time and writes the device status to InfluxDB. The Last Will payload does not contain a timestamp because it is prepared before an unexpected disconnection occurs.

## General rules

- All payloads use UTF-8 JSON.
- Field names are case-sensitive and use `snake_case`.
- Measurements must be JSON numbers, not strings.
- `NaN` and infinity must never be published.
- The payload `device_id` must match the device ID in the MQTT topic.
- Invalid common metadata rejects the complete raw message.
- A sensor error rejects only the affected measurement.
- The current contract version is `v1`.
- Passwords, Wi-Fi credentials and tokens must never appear in MQTT payloads or be committed to Git.
