idf_component_register(
    SRCS "tuya_client.cpp" "tuya_http.cpp"
    INCLUDE_DIRS "include"
    REQUIRES config_manager crypto_manager http_server
             esp_http_client mbedtls mbedtls log freertos
)
