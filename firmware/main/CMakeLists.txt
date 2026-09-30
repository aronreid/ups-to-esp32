idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board netmgr ups_hid nut_server webui display nvs_flash fleet crashlog auth esp_driver_uart
)
