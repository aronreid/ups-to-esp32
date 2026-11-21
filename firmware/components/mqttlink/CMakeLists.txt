idf_component_register(
    SRCS "mqttlink.c" "mqtt_core.c"
    INCLUDE_DIRS "include"
    REQUIRES outputs mqtt esp_event
    PRIV_REQUIRES ups_hid netmgr board nvs_flash esp_timer esp_app_format
)
