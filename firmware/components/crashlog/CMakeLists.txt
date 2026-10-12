# The Xtensa component exists only on Xtensa chips (S3); the RISC-V P4 has none.
idf_build_get_property(target_arch IDF_TARGET_ARCH)
set(arch_reqs)
if(target_arch STREQUAL "xtensa")
    set(arch_reqs xtensa)
endif()

idf_component_register(
    SRCS "crashlog.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system esp_hw_support esp_app_format nvs_flash ${arch_reqs}
)
# Every panic -- exceptions, aborts, the interrupt and task watchdogs -- goes
# through esp_panic_handler(). Wrapping it lets crashlog note where, then hand
# over unchanged. See crashlog.c.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
