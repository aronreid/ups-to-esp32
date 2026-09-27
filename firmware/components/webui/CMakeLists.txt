idf_component_register(
    SRCS "webui.c" "metrics.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_http_server fleet crashlog esp_timer ups_hid netmgr nut_server ota board esp_app_format app_update json
)

# The page is stored gzipped and served as it is, with Content-Encoding: gzip:
# 51 KB of HTML is about 17 KB in flash. Every browser this page supports
# asks for gzip. The repository keeps the readable file; this is made at build.
idf_build_get_property(python PYTHON)
set(page_gz "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(
    OUTPUT "${page_gz}"
    COMMAND ${python} "${CMAKE_CURRENT_SOURCE_DIR}/www/gzip_page.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/www/index.html" "${page_gz}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/www/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/www/gzip_page.py"
    VERBATIM)
add_custom_target(webui_page_gz DEPENDS "${page_gz}")
add_dependencies(${COMPONENT_LIB} webui_page_gz)
target_add_binary_data(${COMPONENT_LIB} "${page_gz}" BINARY DEPENDS webui_page_gz)
