idf_component_register(
    SRCS "web_tls.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_system nvs_flash mbedtls
)
