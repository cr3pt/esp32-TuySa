idf_component_register(
    SRCS "webhook_client.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client event_log
)
