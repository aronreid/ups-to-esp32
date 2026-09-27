idf_component_register(
    SRCS "fleet.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_http_client esp-tls mbedtls json esp_app_format
             esp_timer esp_wifi esp_hw_support spi_flash app_update
             board netmgr ota ups_hid nut_server crashlog
)
