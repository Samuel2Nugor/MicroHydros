/*
 * MicroHydros firmware — ESP32-S3 DevKitC-1.1
 * Framework : ESP-IDF v5.x
 *
 * Sensors
 *   SHT31   — I2C  (GPIO8=SDA, GPIO9=SCL)  → internal temp + humidity
 *   DS18B20 — 1-Wire (GPIO4, 4.7kΩ pull-up) → water temp + external air temp
 *
 * Data flow
 *   Reads sensors every CONFIG_TELEMETRY_INTERVAL_SEC seconds.
 *   Publishes one raw telemetry JSON to MQTT per data contract v1.
 *   Topic: microhydros/v1/devices/{device_id}/telemetry/raw
 *
 * Credentials
 *   Set WiFi SSID/password and MQTT broker URL via: idf.py menuconfig
 *   → MicroHydros Configuration
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"

#include "driver/i2c_master.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "onewire_bus.h"
#include "ds18b20.h"

/* ── Tags ─────────────────────────────────────────────────────────────────── */
static const char *TAG        = "microhydros";
static const char *TAG_WIFI   = "wifi";
static const char *TAG_MQTT   = "mqtt";
static const char *TAG_SENSOR = "sensor";

/* ── Configuration (set via menuconfig) ───────────────────────────────────── */
#define WIFI_SSID               CONFIG_WIFI_SSID
#define WIFI_PASS               CONFIG_WIFI_PASSWORD
#define MQTT_BROKER_URL         CONFIG_MQTT_BROKER_URL
#define DEVICE_ID               CONFIG_DEVICE_ID
#define ONEWIRE_GPIO            CONFIG_ONEWIRE_GPIO
#define I2C_SDA                 CONFIG_I2C_SDA_GPIO
#define I2C_SCL                 CONFIG_I2C_SCL_GPIO
#define TELEMETRY_INTERVAL_US   (CONFIG_TELEMETRY_INTERVAL_SEC * 1000ULL * 1000ULL)

/* ── SHT31 ────────────────────────────────────────────────────────────────── */
#define SHT31_I2C_ADDR          0x44
#define SHT31_CMD_MEAS_H        0x2C   /* high repeatability, clock stretch */
#define SHT31_CMD_MEAS_L        0x06

/* ── DS18B20 roles ────────────────────────────────────────────────────────── */
#define DS18B20_MAX             2
#define DS18B20_ROLE_WATER      0
#define DS18B20_ROLE_EXTERNAL   1

/* Plausibility ranges live in Node-RED (PLAUSIBLE_RANGES) — single source of
 * truth. Firmware reports only transaction status (ok / read_error /
 * not_detected); the validator rejects out-of-range values per field. */

/* ── WiFi event group ─────────────────────────────────────────────────────── */
#define WIFI_CONNECTED_BIT      BIT0
static EventGroupHandle_t s_wifi_event_group;

/* ── Global state ─────────────────────────────────────────────────────────── */
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool                     s_mqtt_connected = false;
static char                     s_boot_id[9];      /* 8 hex chars + null */
static uint32_t                 s_sequence = 0;

static i2c_master_bus_handle_t  s_i2c_bus = NULL;
static i2c_master_dev_handle_t  s_sht31_dev = NULL;
static bool                     s_sht31_ok = false;

static onewire_bus_handle_t     s_ow_bus = NULL;
static ds18b20_device_handle_t  s_ds18b20[DS18B20_MAX];       /* indexed by ROLE */
static bool                     s_ds18b20_present[DS18B20_MAX];

/* ── Sensor reading result ────────────────────────────────────────────────── */
typedef struct {
    float    value;
    bool     valid;
    char     status[16];   /* "ok" | "read_error" | "not_detected" */
} sensor_reading_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * WiFi
 * ═════════════════════════════════════════════════════════════════════════ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG_WIFI, "disconnected — retrying");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG_WIFI, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any, inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_got_ip));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG_WIFI, "connecting to %s", WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_WIFI, "connected");
    } else {
        ESP_LOGW(TAG_WIFI, "not connected within 30s — continuing, will retry in background");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MQTT
 * ═════════════════════════════════════════════════════════════════════════ */

static void build_status_payload(char *buf, size_t len, const char *status)
{
    snprintf(buf, len,
        "{\"schema_version\":1,\"device_id\":\"%s\","
        "\"boot_id\":\"%s\",\"status\":\"%s\"}",
        DEVICE_ID, s_boot_id, status);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    char status_topic[80];
    char status_payload[128];

    snprintf(status_topic, sizeof(status_topic),
             "microhydros/v1/devices/%s/status", DEVICE_ID);

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG_MQTT, "connected to broker");
        s_mqtt_connected = true;
        build_status_payload(status_payload, sizeof(status_payload), "online");
        esp_mqtt_client_publish(s_mqtt_client, status_topic,
                                status_payload, 0, 1, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG_MQTT, "disconnected");
        s_mqtt_connected = false;
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG_MQTT, "error");
        break;
    default:
        break;
    }
}

static void mqtt_init(void)
{
    char status_topic[80];
    char lwt_payload[128];

    snprintf(status_topic, sizeof(status_topic),
             "microhydros/v1/devices/%s/status", DEVICE_ID);
    build_status_payload(lwt_payload, sizeof(lwt_payload), "offline");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri     = MQTT_BROKER_URL,
        .session.last_will = {
            .topic   = status_topic,
            .msg     = lwt_payload,
            .msg_len = strlen(lwt_payload),
            .qos     = 1,
            .retain  = 1,
        },
    };

    s_mqtt_client = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SHT31
 * ═════════════════════════════════════════════════════════════════════════ */

static uint8_t sht31_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void sht31_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port      = I2C_NUM_0,
        .sda_io_num    = I2C_SDA,
        .scl_io_num    = I2C_SCL,
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG_SENSOR, "SHT31: I2C bus init failed");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SHT31_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_sht31_dev) != ESP_OK) {
        ESP_LOGE(TAG_SENSOR, "SHT31: device add failed");
        return;
    }

    /* probe */
    uint8_t cmd[2] = {SHT31_CMD_MEAS_H, SHT31_CMD_MEAS_L};
    uint8_t buf[6];
    esp_err_t err = i2c_master_transmit(s_sht31_dev, cmd, 2, 100);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        err = i2c_master_receive(s_sht31_dev, buf, 6, 100);
    }
    s_sht31_ok = (err == ESP_OK);
    ESP_LOGI(TAG_SENSOR, "SHT31 @0x44: %s", s_sht31_ok ? "detected" : "NOT detected");
}

static void sht31_read(sensor_reading_t *temp, sensor_reading_t *humidity)
{
    temp->valid = false;  humidity->valid = false;
    strlcpy(temp->status,     "not_detected", sizeof(temp->status));
    strlcpy(humidity->status, "not_detected", sizeof(humidity->status));

    if (!s_sht31_ok) return;

    uint8_t cmd[2] = {SHT31_CMD_MEAS_H, SHT31_CMD_MEAS_L};
    uint8_t buf[6];

    if (i2c_master_transmit(s_sht31_dev, cmd, 2, 100) != ESP_OK) goto read_error;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (i2c_master_receive(s_sht31_dev, buf, 6, 100) != ESP_OK)  goto read_error;
    if (sht31_crc8(&buf[0], 2) != buf[2] ||
        sht31_crc8(&buf[3], 2) != buf[5])                        goto read_error;

    uint16_t raw_t = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_h = ((uint16_t)buf[3] << 8) | buf[4];

    /* Transaction + CRC ok → both channels "ok". Plausibility is the
     * validator's job. Folding it in here would let an implausible temperature
     * reject a valid humidity reading (they share one sensor_status field). */
    temp->value     = -45.0f + 175.0f * raw_t / 65535.0f;
    humidity->value = 100.0f * raw_h / 65535.0f;
    temp->valid = true;  humidity->valid = true;
    strlcpy(temp->status,     "ok", sizeof(temp->status));
    strlcpy(humidity->status, "ok", sizeof(humidity->status));
    return;

read_error:
    strlcpy(temp->status,     "read_error", sizeof(temp->status));
    strlcpy(humidity->status, "read_error", sizeof(humidity->status));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DS18B20
 * ═════════════════════════════════════════════════════════════════════════ */

static void ds18b20_init_sensors(void)
{
    for (int i = 0; i < DS18B20_MAX; i++) {
        s_ds18b20[i] = NULL;
        s_ds18b20_present[i] = false;
    }

    const uint64_t role_rom[DS18B20_MAX] = {
        [DS18B20_ROLE_WATER]    = strtoull(CONFIG_DS18B20_WATER_ROM,    NULL, 16),
        [DS18B20_ROLE_EXTERNAL] = strtoull(CONFIG_DS18B20_EXTERNAL_ROM, NULL, 16),
    };

    onewire_bus_config_t bus_cfg = { .bus_gpio_num = ONEWIRE_GPIO };
    onewire_bus_rmt_config_t rmt_cfg = { .max_rx_bytes = 10 };
    if (onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_ow_bus) != ESP_OK) {
        ESP_LOGE(TAG_SENSOR, "1-Wire bus init failed — skipping DS18B20");
        return;
    }

    onewire_device_iter_handle_t iter;
    if (onewire_new_device_iter(s_ow_bus, &iter) != ESP_OK) {
        ESP_LOGE(TAG_SENSOR, "1-Wire iter create failed — skipping DS18B20");
        return;
    }

    /* Discover every probe first, keeping handle + ROM together. */
    struct { ds18b20_device_handle_t h; uint64_t rom; } found[DS18B20_MAX];
    int found_count = 0;

    onewire_device_t device;
    esp_err_t err;
    do {
        err = onewire_device_iter_get_next(iter, &device);
        if (err == ESP_OK && found_count < DS18B20_MAX) {
            ds18b20_config_t ds_cfg = {};
            ds18b20_device_handle_t h;
            if (ds18b20_new_device(&device, &ds_cfg, &h) == ESP_OK) {
                found[found_count].h   = h;
                found[found_count].rom = device.address;
                ESP_LOGI(TAG_SENSOR, "DS18B20 discovered ROM: %016llX", device.address);
                found_count++;
            }
        }
    } while (err != ESP_ERR_NOT_FOUND);
    onewire_del_device_iter(iter);

    bool used[DS18B20_MAX] = { false };

    /* Pass 1: assign pinned roles by matching ROM. */
    for (int r = 0; r < DS18B20_MAX; r++) {
        if (role_rom[r] == 0) continue;
        for (int i = 0; i < found_count; i++) {
            if (!used[i] && found[i].rom == role_rom[r]) {
                s_ds18b20[r] = found[i].h;
                s_ds18b20_present[r] = true;
                used[i] = true;
                break;
            }
        }
        if (!s_ds18b20_present[r]) {
            ESP_LOGW(TAG_SENSOR, "%s ROM %016llX not on bus",
                     r == DS18B20_ROLE_WATER ? "water" : "external", role_rom[r]);
        }
    }

    /* Pass 2: fallback for any unpinned/missing role — assign remaining probe
     * by discovery order, but WARN loudly so a swap can't hide. */
    for (int r = 0; r < DS18B20_MAX; r++) {
        if (s_ds18b20_present[r]) continue;
        for (int i = 0; i < found_count; i++) {
            if (!used[i]) {
                s_ds18b20[r] = found[i].h;
                s_ds18b20_present[r] = true;
                used[i] = true;
                ESP_LOGW(TAG_SENSOR,
                    "%s role UNPINNED — using ROM %016llX by order; "
                    "set CONFIG_DS18B20_%s_ROM to pin it",
                    r == DS18B20_ROLE_WATER ? "water" : "external", found[i].rom,
                    r == DS18B20_ROLE_WATER ? "WATER" : "EXTERNAL");
                break;
            }
        }
    }

    ESP_LOGI(TAG_SENSOR, "DS18B20 roles: water=%s external=%s",
             s_ds18b20_present[DS18B20_ROLE_WATER]    ? "ready" : "MISSING",
             s_ds18b20_present[DS18B20_ROLE_EXTERNAL] ? "ready" : "MISSING");
}

static void ds18b20_read_sensor(int role, sensor_reading_t *result)
{
    result->valid = false;
    strlcpy(result->status, "not_detected", sizeof(result->status));

    if (role < 0 || role >= DS18B20_MAX || !s_ds18b20_present[role]) return;

    ds18b20_trigger_temperature_conversion(s_ds18b20[role]);
    vTaskDelay(pdMS_TO_TICKS(800));  /* DS18B20 max conversion time at 12-bit */

    float temp;
    if (ds18b20_get_temperature(s_ds18b20[role], &temp) != ESP_OK) {
        strlcpy(result->status, "read_error", sizeof(result->status));
        return;
    }

    /* Plausibility is the validator's job — emit the raw value. */
    result->value = temp;
    result->valid = true;
    strlcpy(result->status, "ok", sizeof(result->status));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MQTT payload builder
 * ═════════════════════════════════════════════════════════════════════════ */

static void publish_telemetry(sensor_reading_t *internal_temp,
                              sensor_reading_t *internal_hum,
                              sensor_reading_t *water_temp,
                              sensor_reading_t *external_temp)
{
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG_MQTT, "not connected — skipping publish");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG_MQTT, "cJSON alloc failed — skipping publish");
        return;
    }
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "device_id",      DEVICE_ID);
    cJSON_AddStringToObject(root, "boot_id",        s_boot_id);
    cJSON_AddNumberToObject(root, "sequence",       s_sequence++);
    cJSON_AddNumberToObject(root, "uptime_ms",      (double)(esp_timer_get_time() / 1000));

    cJSON *measurements = cJSON_AddObjectToObject(root, "measurements");
    if (internal_temp->valid)
        cJSON_AddNumberToObject(measurements, "internal_temperature_c",  internal_temp->value);
    else
        cJSON_AddNullToObject(measurements,  "internal_temperature_c");

    if (internal_hum->valid)
        cJSON_AddNumberToObject(measurements, "internal_humidity_percent", internal_hum->value);
    else
        cJSON_AddNullToObject(measurements,   "internal_humidity_percent");

    if (external_temp->valid)
        cJSON_AddNumberToObject(measurements, "external_temperature_c",  external_temp->value);
    else
        cJSON_AddNullToObject(measurements,   "external_temperature_c");

    if (water_temp->valid)
        cJSON_AddNumberToObject(measurements, "water_temperature_c",     water_temp->value);
    else
        cJSON_AddNullToObject(measurements,   "water_temperature_c");

    cJSON *sensor_status = cJSON_AddObjectToObject(root, "sensor_status");
    cJSON_AddStringToObject(sensor_status, "internal_sht31",    internal_temp->status);
    cJSON_AddStringToObject(sensor_status, "water_ds18b20",     water_temp->status);
    cJSON_AddStringToObject(sensor_status, "external_ds18b20",  external_temp->status);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        ESP_LOGE(TAG_MQTT, "cJSON print failed — skipping publish");
        return;
    }

    char topic[80];
    snprintf(topic, sizeof(topic),
             "microhydros/v1/devices/%s/telemetry/raw", DEVICE_ID);

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG_MQTT, "published (msg_id=%d) seq=%"PRIu32, msg_id, s_sequence - 1);
    ESP_LOGI(TAG_MQTT, "%s", payload);

    free(payload);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Telemetry task — sensor work runs here, not in esp_timer callback,
 * to avoid blocking the timer task with DS18B20 conversion delays (~1.6 s).
 * ═════════════════════════════════════════════════════════════════════════ */

static TaskHandle_t s_telemetry_task = NULL;

static void telemetry_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        sensor_reading_t internal_temp, internal_hum, water_temp, external_temp;

        sht31_read(&internal_temp, &internal_hum);
        ds18b20_read_sensor(DS18B20_ROLE_WATER,    &water_temp);
        ds18b20_read_sensor(DS18B20_ROLE_EXTERNAL, &external_temp);

        ESP_LOGI(TAG_SENSOR, "internal_temp=%.2f(%s) internal_hum=%.1f(%s) water=%.2f(%s) external=%.2f(%s)",
                 internal_temp.value,  internal_temp.status,
                 internal_hum.value,   internal_hum.status,
                 water_temp.value,     water_temp.status,
                 external_temp.value,  external_temp.status);

        publish_telemetry(&internal_temp, &internal_hum, &water_temp, &external_temp);
    }
}

static void telemetry_timer_cb(void *arg)
{
    if (s_telemetry_task) {
        xTaskNotifyGive(s_telemetry_task);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * app_main
 * ═════════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    /* Generate boot_id from random 32-bit value */
    snprintf(s_boot_id, sizeof(s_boot_id), "%08"PRIx32, esp_random());
    ESP_LOGI(TAG, "MicroHydros boot_id=%s device_id=%s", s_boot_id, DEVICE_ID);

    /* NVS */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* Sensors */
    sht31_init();
    ds18b20_init_sensors();

    /* Network */
    wifi_init();
    mqtt_init();

    /* Wait up to 60 s for MQTT — start timer regardless; client auto-reconnects */
    for (int i = 0; i < 120 && !s_mqtt_connected; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG_MQTT, "not connected within 60s — continuing, will retry in background");
    }

    /* Sensor task — receives notify from timer, does the actual work */
    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 5, &s_telemetry_task);

    /* Periodic telemetry timer */
    const esp_timer_create_args_t timer_args = {
        .callback = telemetry_timer_cb,
        .name     = "telemetry",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, TELEMETRY_INTERVAL_US));

    /* Trigger first reading immediately */
    telemetry_timer_cb(NULL);

    ESP_LOGI(TAG, "running — publishing every %d s", CONFIG_TELEMETRY_INTERVAL_SEC);
}
