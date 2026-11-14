set(SRCS
    "src/microlink.c"
    "src/ml_coord.c"
    "src/ml_derp.c"
    "src/ml_net_io.c"
    "src/ml_wg_mgr.c"
    "src/ml_stun.c"
    "src/ml_netcheck.c"
    "src/ml_noise.c"
    "src/ml_h2.c"
    "src/ml_udp.c"
    "src/ml_tcp.c"
    "src/ml_peer_nvs.c"
    "src/nacl_box.c"
    "src/ml_x25519.c"
    "src/ml_zerocopy.c"
    "src/ml_cellular.c"
    "src/ml_at_socket.c"
    "src/ml_net_switch.c"
    "src/ml_config_httpd.c"
)

# ups-driver local patch: no sources unless Tailscale is configured.
# ESP-IDF builds every component it finds under components/, so without this a
# Rev A build would compile MicroLink too. Only SRCS is conditional: CONFIG_
# values are not yet known when IDF first walks REQUIRES, so a conditional
# REQUIRES list comes out empty and the real build then misses its headers.
# The requirements are all components the build contains anyway.
if(NOT CONFIG_UPSA_TAILSCALE)
    set(SRCS)
endif()

set(REQUIRES
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    lwip
    mbedtls
    json
    driver
    esp_driver_tsens
    esp_http_server
    esp-tls
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES ${REQUIRES}
    PRIV_REQUIRES
        wireguard_lwip
)

# ups-driver local patch: upstream's PlatformIO-only include hack for esp-tls
# is dropped. Native ESP-IDF propagates it through REQUIRES esp-tls, and the
# variable it used is empty here.
