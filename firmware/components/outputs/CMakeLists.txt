idf_component_register(
    SRCS "outputs.c"
    INCLUDE_DIRS "include"
    REQUIRES board nvs_flash esp_timer esp_driver_gpio esp_system
)
