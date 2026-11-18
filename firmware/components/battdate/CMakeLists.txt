idf_component_register(
    SRCS "battdate.c"
    INCLUDE_DIRS "include"
    REQUIRES ups_hid nvs_flash
)
