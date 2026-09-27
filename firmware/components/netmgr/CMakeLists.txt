idf_component_register(
    SRCS "netmgr.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif nvs_flash esp_event esp_timer board mdns
)
