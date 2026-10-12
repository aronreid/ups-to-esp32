idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board netmgr ups_hid nut_server webui display tailnet outagelog history nvs_flash fleet crashlog auth esp_driver_uart app_update
)
