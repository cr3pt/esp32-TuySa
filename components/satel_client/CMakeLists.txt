idf_component_register(
    SRCS "satel_client.cpp" "satel_protocol.cpp"
    INCLUDE_DIRS "include"
    REQUIRES config_manager crypto_manager http_server
             lwip esp_http_client esp_log freertos
)
