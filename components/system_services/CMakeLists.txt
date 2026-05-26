idf_component_register(
    SRCS "system_services.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif mdns lwip esp_timer esp_event
)
