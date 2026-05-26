#include "boot_manager.h"
#include "config_manager.h"
#include "crypto_manager.h"
#include "tuya_client.h"
#include "satel_client.h"
#include "rule_engine.h"
#include "default_rules.h"
#include "watchdog.h"
#include "wifi_manager.h"
#include "captive_portal.h"
#include "spiffs_www.h"
#include "http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
static const char *TAG="MAIN";
static void on_config_saved(void){ boot_manager_restart("portal-saved",1500); }
static esp_err_t on_test(const char *target){ if(strcmp(target,"tuya")==0) return tuya_client_test(); if(strcmp(target,"satel")==0) return satel_client_test(); return ESP_ERR_INVALID_ARG; }
static esp_err_t on_set_key(const char *pass){ return crypto_manager_set_key_from_passphrase(pass, pass?strlen(pass):0); }
static void on_factory_reset(void){ watchdog_stop(); rule_engine_stop(); tuya_client_stop(); satel_client_stop(); crypto_erase_secrets(); config_manager_erase_all(); boot_manager_restart("factory-reset", 500); }
extern "C" void app_main(void){ boot_mode_t mode=boot_manager_init(); ESP_ERROR_CHECK(wifi_manager_init()); if(mode==BOOT_MODE_SETUP){ ESP_ERROR_CHECK(wifi_manager_start_ap("ESP32-Bridge-Setup")); ESP_ERROR_CHECK(captive_portal_start(on_config_saved)); while(true) vTaskDelay(pdMS_TO_TICKS(10000)); } if(wifi_manager_start_sta(15000)!=ESP_OK){ ESP_LOGE(TAG,"WiFi timeout"); boot_manager_restart("wifi-fail", 2000); } ESP_ERROR_CHECK(spiffs_www_mount()); http_server_set_test_cb(on_test); http_server_set_reset_cb(on_factory_reset); http_server_set_key_cb(on_set_key); http_server_set_status_cb(watchdog_health_to_json); ESP_ERROR_CHECK(http_server_start("admin","bridge123")); crypto_manager_init(); if(tuya_client_init()==ESP_OK) tuya_client_start(); if(satel_client_init()==ESP_OK) satel_client_start(); ESP_ERROR_CHECK(rule_engine_init()); rule_engine_load_defaults(); ESP_ERROR_CHECK(rule_engine_start()); http_server_set_rules_api_cb(rule_engine_rules_to_json, rule_engine_add_from_json, rule_engine_delete, rule_engine_test_fire); ESP_ERROR_CHECK(watchdog_init()); ESP_ERROR_CHECK(watchdog_start()); while(true){ vTaskDelay(pdMS_TO_TICKS(30000)); rule_engine_save(); } }
