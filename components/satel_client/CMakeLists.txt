idf_component_register(
    SRCS "satel_client.cpp" "satel_protocol.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        config_manager
        crypto_manager
        http_server
        esp_log
        esp_timer
        freertos
        lwip
)
