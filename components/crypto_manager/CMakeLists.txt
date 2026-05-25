idf_component_register(
    SRCS "crypto_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        config_manager
        esp_log
        mbedtls
        nvs_flash
)
