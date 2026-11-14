# Built only with CONFIG_UPSA_TLS (Rev B); the headers are always exported so
# callers can include them, and they guard their calls with the same symbol.
# Only SRCS is conditional, as in outagelog: CONFIG_ values are not yet known
# when IDF first walks REQUIRES.
set(srcs)
if(CONFIG_UPSA_TLS)
    set(srcs "tlscert.c" "tlscert_core.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls nvs_flash esp_hw_support esp_app_format netmgr
)
