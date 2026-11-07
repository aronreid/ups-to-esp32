idf_component_register(
    SRCS "snmp.c" "snmp_core.c"
    INCLUDE_DIRS "include"
    REQUIRES ups_hid nut_server netmgr board nvs_flash esp_timer esp_app_format lwip
)
