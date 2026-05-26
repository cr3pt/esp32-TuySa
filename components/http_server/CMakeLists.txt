idf_component_register(
    SRCS "http_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server system_services web_tls ota_manager automation_modes rate_limit hw_watchdog event_log mqtt_bridge webhook_client config_manager log mbedtls esp_hw_support nvs_flash mdns_wrapper
)
