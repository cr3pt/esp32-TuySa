#include "config_manager.h"
#include "nvs_flash.h"
#include "nvs.h"

esp_err_t config_manager_save_net(const net_config_t *c){nvs_handle_t h; if(nvs_open("bridge_cfg",NVS_READWRITE,&h)!=ESP_OK)return ESP_FAIL; nvs_set_blob(h,"net_cfg",c,sizeof(*c)); nvs_commit(h); nvs_close(h); return ESP_OK;}
esp_err_t config_manager_load_net(net_config_t *c){nvs_handle_t h; if(nvs_open("bridge_cfg",NVS_READONLY,&h)!=ESP_OK)return ESP_ERR_NOT_FOUND; size_t sz=sizeof(*c); esp_err_t e=nvs_get_blob(h,"net_cfg",c,&sz); nvs_close(h); return e;}
esp_err_t config_manager_save_panel_auth(const panel_auth_t *c){nvs_handle_t h; if(nvs_open("bridge_cfg",NVS_READWRITE,&h)!=ESP_OK)return ESP_FAIL; nvs_set_blob(h,"panel_auth",c,sizeof(*c)); nvs_commit(h); nvs_close(h); return ESP_OK;}
esp_err_t config_manager_load_panel_auth(panel_auth_t *c){nvs_handle_t h; if(nvs_open("bridge_cfg",NVS_READONLY,&h)!=ESP_OK)return ESP_ERR_NOT_FOUND; size_t sz=sizeof(*c); esp_err_t e=nvs_get_blob(h,"panel_auth",c,&sz); nvs_close(h); return e;}
esp_err_t config_manager_erase_all(void){return nvs_flash_erase();}
