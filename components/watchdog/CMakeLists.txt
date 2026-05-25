idf_component_register(
    SRCS "watchdog.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        tuya_client
        satel_client
        wifi_manager
        http_server
        driver
        esp_timer
        esp_log
        freertos
)
