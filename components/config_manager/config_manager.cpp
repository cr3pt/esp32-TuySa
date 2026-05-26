#include "config_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
static esp_err_t open_rw(nvs_handle_t *h){return nvs_open("bridge_cfg",NVS_READWRITE,h);} 
static esp_err_t open_ro(nvs_handle_t *h){return nvs_open("bridge_cfg",NVS_READONLY,h);} 
esp_err_t config_manager_save_net(const net_config_t *c){nvs_handle_t h; esp_err_t e=open_rw(&h); if(e)return e; nvs_set_blob(h,"net_cfg",c,sizeof(*c)); e=nvs_commit(h); nvs_close(h); return e;} 
esp_err_t config_manager_load_net(net_config_t *c){nvs_handle_t h; esp_err_t e=open_ro(&h); if(e)return e; size_t sz=sizeof(*c); e=nvs_get_blob(h,"net_cfg",c,&sz); nvs_close(h); return e;} 
esp_err_t config_manager_save_tuya(const tuya_creds_t *c){nvs_handle_t h; esp_err_t e=open_rw(&h); if(e)return e; nvs_set_blob(h,"tuya_creds",c,sizeof(*c)); e=nvs_commit(h); nvs_close(h); return e;} 
esp_err_t config_manager_load_tuya(tuya_creds_t *c){nvs_handle_t h; esp_err_t e=open_ro(&h); if(e)return e; size_t sz=sizeof(*c); e=nvs_get_blob(h,"tuya_creds",c,&sz); nvs_close(h); return e;} 
esp_err_t config_manager_save_satel(const satel_creds_t *c){nvs_handle_t h; esp_err_t e=open_rw(&h); if(e)return e; nvs_set_blob(h,"satel_creds",c,sizeof(*c)); e=nvs_commit(h); nvs_close(h); return e;} 
esp_err_t config_manager_load_satel(satel_creds_t *c){nvs_handle_t h; esp_err_t e=open_ro(&h); if(e)return e; size_t sz=sizeof(*c); e=nvs_get_blob(h,"satel_creds",c,&sz); nvs_close(h); return e;} 
esp_err_t config_manager_erase_all(void){return nvs_flash_erase();}
