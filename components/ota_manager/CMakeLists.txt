idf_component_register(
    SRCS "ota_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_https_ota esp_http_client esp_crt_bundle nvs_flash
)
