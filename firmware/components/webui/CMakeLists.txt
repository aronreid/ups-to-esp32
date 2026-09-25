idf_component_register(
    SRCS "webui.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_http_server ups_hid netmgr nut_server ota board esp_app_format app_update json
    EMBED_FILES "www/index.html"
)
