idf_component_register(
    SRCS "rule_engine.cpp"
    INCLUDE_DIRS "include"
    REQUIRES log freertos config_manager tuya_client satel_client automation_modes mqtt_bridge webhook_client event_log system_services
)
