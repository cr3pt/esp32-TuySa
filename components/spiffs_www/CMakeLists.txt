idf_component_register(
    SRCS "spiffs_www.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_server
        spiffs
        esp_log
)
