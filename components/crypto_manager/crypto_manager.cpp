#include "crypto_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
static bool s_ready=false;
esp_err_t crypto_load_satel_creds(satel_creds_t *c,size_t sz){nvs_handle_t h; if(nvs_open("bridge_crypto",NVS_READONLY,&h)!=ESP_OK)return ESP_ERR_NOT_FOUND; size_t bsz=sz; esp_err_t e=nvs_get_blob(h,"satel",c,&bsz); nvs_close(h); return e;}
esp_err_t crypto_save_satel_creds(const satel_creds_t *c,size_t sz){nvs_handle_t h; if(nvs_open("bridge_crypto",NVS_READWRITE,&h)!=ESP_OK)return ESP_FAIL; nvs_set_blob(h,"satel",c,sz); nvs_commit(h); nvs_close(h); return ESP_OK;}
esp_err_t crypto_load_tuya_creds(tuya_creds_t *c,size_t sz){nvs_handle_t h; if(nvs_open("bridge_crypto",NVS_READONLY,&h)!=ESP_OK)return ESP_ERR_NOT_FOUND; size_t bsz=sz; esp_err_t e=nvs_get_blob(h,"tuya",c,&bsz); nvs_close(h); return e;}
esp_err_t crypto_save_tuya_creds(const tuya_creds_t *c,size_t sz){nvs_handle_t h; if(nvs_open("bridge_crypto",NVS_READWRITE,&h)!=ESP_OK)return ESP_FAIL; nvs_set_blob(h,"tuya",c,sz); nvs_commit(h); nvs_close(h); return ESP_OK;}
esp_err_t crypto_manager_set_key_from_passphrase(const char *,size_t){s_ready=true; return ESP_OK;}
void crypto_erase_secrets(void){nvs_flash_erase(); s_ready=false;}
bool crypto_manager_is_ready(void){return s_ready;}
