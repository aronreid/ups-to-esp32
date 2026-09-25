idf_component_register(
    SRCS "nut_server.c"
    INCLUDE_DIRS "include"
    REQUIRES ups_hid esp_netif lwip esp_app_format
)
