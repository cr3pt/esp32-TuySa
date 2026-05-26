idf_component_register(
    SRCS "mqtt_bridge.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_mqtt esp_event esp_netif event_log
)
