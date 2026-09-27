idf_component_register(
    SRCS "crashlog.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system esp_hw_support esp_app_format nvs_flash xtensa
)
# Every panic -- exceptions, aborts, the interrupt and task watchdogs -- goes
# through esp_panic_handler(). Wrapping it lets crashlog note where, then hand
# over unchanged. See crashlog.c.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
