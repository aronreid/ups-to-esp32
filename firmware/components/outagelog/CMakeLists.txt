# Built only with CONFIG_UPSA_OUTAGE_LOG (Rev B); the headers are always
# exported so callers can include them, and they guard their calls with the
# same symbol. Only SRCS is conditional: CONFIG_ values are not yet known when
# IDF first walks REQUIRES, so a conditional REQUIRES list comes out empty.
set(srcs)
if(CONFIG_UPSA_OUTAGE_LOG)
    set(srcs "outagelog.c" "outagelog_core.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ups_hid esp_partition esp_netif esp_timer nvs_flash lwip
)
