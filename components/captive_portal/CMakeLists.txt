idf_component_register(
    SRCS "captive_portal.cpp" "dns_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        config_manager
        wifi_manager
        esp_http_server
        lwip
        esp_log
        freertos
)
