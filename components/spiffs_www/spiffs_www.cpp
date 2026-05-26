#include "spiffs_www.h"
#include "esp_spiffs.h"
esp_err_t spiffs_www_mount(void){ esp_vfs_spiffs_conf_t cfg={"/www","spiffs",10,true}; esp_vfs_spiffs_register(&cfg); return ESP_OK; }
