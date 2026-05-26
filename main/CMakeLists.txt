idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES satel_client tuya_client crypto_manager config_manager
             system_services ota_manager web_tls mqtt_bridge webhook_client event_log
             automation_modes rate_limit hw_watchdog http_server
             nvs_flash log freertos
)
