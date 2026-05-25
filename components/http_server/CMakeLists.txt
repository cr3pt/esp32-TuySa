idf_component_register(
    SRCS "http_server.cpp" "http_server_crypto_patch.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        config_manager
        crypto_manager
        wifi_manager
        esp_http_server
        spiffs_www
        esp_log
        freertos
        mbedtls
)
