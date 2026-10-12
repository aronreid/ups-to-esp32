# Wi-Fi boards build netmgr.c; boards with no radio (CONFIG_UPSA_NET_ETHERNET)
# build netmgr_eth.c, the same netmgr.h over a cable. Only SRCS is conditional:
# CONFIG_ values are not yet known when IDF first walks REQUIRES.
if(CONFIG_UPSA_NET_ETHERNET)
    set(srcs "netmgr_eth.c")
else()
    set(srcs "netmgr.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_eth esp_netif nvs_flash esp_event esp_timer board mdns lwip
)
