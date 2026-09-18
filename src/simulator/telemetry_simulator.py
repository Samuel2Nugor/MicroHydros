import random
import time
import json
import paho.mqtt.client as mqtt

BROKER_HOST = "localhost"
BROKER_PORT = 1883
DEVICE_ID = "simulator-01"
TOPIC = f"microhydros/v1/devices/{DEVICE_ID}/telemetry/raw"

client = mqtt.Client()
client.connect(BROKER_HOST, BROKER_PORT)

sequence = 0  # räknar upp för varje skickat meddelande

try:
    while True:
        uptime_ms = sequence * 5000  # 1 sequence = 5 sekunder "igång"

        payload = {
            "schema_version": 1,
            "device_id": DEVICE_ID,
            "boot_id": "sim-boot-1",
            "sequence": sequence,
            "uptime_ms": uptime_ms,
            "measurements": {
                # + random.uniform(...) ger lite naturlig variation, som en riktig sensor
                "internal_temperature_c": round(23.5 + random.uniform(-0.5, 0.5), 2),
                "internal_humidity_percent": round(60.0 + random.uniform(-2, 2), 2),
                "external_temperature_c": round(15.0 + random.uniform(-1, 1), 2),
                "water_temperature_c": round(20.0 + random.uniform(-0.3, 0.3), 2)
            },
            "sensor_status": {
                "internal_sht31": "ok",
                "external_ds18b20": "ok",
                "water_ds18b20": "ok"
            }
        }

        client.publish(TOPIC, json.dumps(payload), qos=1)
        print(f"Skickat sequence={sequence}")

        sequence += 1
        time.sleep(5)  # vänta 5 sekund innan nästa meddelande
except KeyboardInterrupt:
    # körs när du trycker Ctrl+C i terminalen
    print("Avbryter...")
finally:
    client.disconnect()  # koppla ner snyggt, även vid avbrott
