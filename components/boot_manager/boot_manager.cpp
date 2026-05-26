#include "boot_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
static const char *TAG="BOOT";
boot_mode_t boot_manager_init(void){ esp_err_t e=nvs_flash_init(); if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){nvs_flash_erase();nvs_flash_init();} nvs_handle_t h; uint8_t setup=1; if(nvs_open("bridge_boot",NVS_READONLY,&h)==ESP_OK){nvs_get_u8(h,"setup_done",&setup); nvs_close(h);} if(setup==1){ESP_LOGI(TAG,"Tryb SETUP"); return BOOT_MODE_SETUP;} ESP_LOGI(TAG,"Tryb NORMAL"); return BOOT_MODE_NORMAL; }
void boot_manager_restart(const char *reason,uint32_t delay_ms){ESP_LOGI(TAG,"Restart: %s za %lums",reason,(unsigned long)delay_ms);vTaskDelay(pdMS_TO_TICKS(delay_ms));esp_restart();}
void boot_manager_clear_setup_flag(void){nvs_handle_t h; if(nvs_open("bridge_boot",NVS_READWRITE,&h)==ESP_OK){nvs_set_u8(h,"setup_done",0);nvs_commit(h);nvs_close(h);}}
