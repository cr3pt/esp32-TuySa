idf_component_register(
    SRCS "wifi_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        config_manager
        esp_wifi
        esp_netif
        esp_event
        esp_log
        lwip
        freertos
)
