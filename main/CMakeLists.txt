idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        boot_manager config_manager crypto_manager
        tuya_client satel_client rule_engine watchdog
        wifi_manager captive_portal
        spiffs_www http_server
        esp_log esp_system esp_sntp freertos nvs_flash driver
)
