idf_component_register(
    SRCS "ups_hid.c" "hid_parser.c" "ups_profiles.c" "ups_varmap.c"
    INCLUDE_DIRS "include"
    REQUIRES usb board esp_timer
)
