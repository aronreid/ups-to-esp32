idf_component_register(
    SRCS "ota.c" "ota_notes.c"
    INCLUDE_DIRS "include"
    REQUIRES board netmgr nvs_flash esp_http_client esp_https_ota app_update esp_app_format
             esp-tls mbedtls json
)
