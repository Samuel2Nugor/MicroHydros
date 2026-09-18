idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash
             driver esp_timer json mqtt
             onewire_bus ds18b20
)
