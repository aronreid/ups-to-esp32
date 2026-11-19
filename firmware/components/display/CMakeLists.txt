idf_component_register(
    SRCS "display.c" "font5x7.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c board ups_hid netmgr nut_server ota
)
