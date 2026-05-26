idf_component_register(SRCS "wifi_manager.cpp" INCLUDE_DIRS "include" REQUIRES esp_wifi esp_netif esp_event esp_log freertos nvs_flash config_manager)
