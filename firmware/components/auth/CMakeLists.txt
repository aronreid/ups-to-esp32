idf_component_register(
    SRCS "auth.c" "auth_core.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer mbedtls
)
