# Built only with CONFIG_UPSA_TAILSCALE; the header is always exported so
# callers can include it, and they guard their calls with the same symbol.
# Only SRCS is conditional: CONFIG_ values are not yet known when IDF first
# walks REQUIRES, so a conditional REQUIRES list comes out empty.
set(srcs)
if(CONFIG_UPSA_TAILSCALE)
    set(srcs "tailnet.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES netmgr
    PRIV_REQUIRES microlink nvs_flash esp_wifi esp_netif esp_event esp_timer
)
