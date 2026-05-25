idf_component_register(
    SRCS "rule_engine.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        config_manager
        tuya_client
        satel_client
        http_server
        nvs_flash
        esp_log
        esp_timer
        freertos
)
