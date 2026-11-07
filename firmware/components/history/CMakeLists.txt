# Built only with CONFIG_UPSA_HISTORY (Rev B); see components/outagelog's
# CMakeLists.txt for why only SRCS is conditional.
set(srcs)
if(CONFIG_UPSA_HISTORY)
    set(srcs "history.c" "history_core.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES outagelog ups_hid esp_partition esp_timer
)
