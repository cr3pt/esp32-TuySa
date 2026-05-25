idf_component_register(
    SRCS "boot_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        config_manager
        nvs_flash
        esp_log
        esp_system
)
