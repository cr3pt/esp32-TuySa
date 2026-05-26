#include "crypto_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/gcm.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include "esp_random.h"
#include <string.h>
#include <stdlib.h>
static const char *NVS_NS="bridge_crypto"; static uint8_t s_key[32]={}; static bool s_ready=false;
static esp_err_t derive_key(const char *pass, size_t plen, const uint8_t *salt, uint8_t *key){mbedtls_md_context_t ctx; mbedtls_md_init(&ctx); const mbedtls_md_info_t *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256); mbedtls_md_setup(&ctx,md,1); int r=mbedtls_pkcs5_pbkdf2_hmac(&ctx,(const uint8_t*)pass,plen,salt,32,10000,32,key); mbedtls_md_free(&ctx); return r==0?ESP_OK:ESP_FAIL;}
esp_err_t crypto_manager_init(void){nvs_handle_t h; if(nvs_open(NVS_NS,NVS_READONLY,&h)!=ESP_OK) return ESP_ERR_NOT_FOUND; uint8_t salt[32]; size_t sz=32; esp_err_t e=nvs_get_blob(h,"salt",salt,&sz); nvs_close(h); return e;}
esp_err_t crypto_manager_set_key_from_passphrase(const char *pass,size_t len){uint8_t salt[32]; esp_fill_random(salt,32); esp_err_t e=derive_key(pass,len,salt,s_key); if(e!=ESP_OK)return e; nvs_handle_t h; if(nvs_open(NVS_NS,NVS_READWRITE,&h)!=ESP_OK)return ESP_FAIL; nvs_set_blob(h,"salt",salt,32); nvs_commit(h); nvs_close(h); s_ready=true; return ESP_OK;}
bool crypto_manager_is_ready(void){return s_ready;}
static esp_err_t encrypt_blob(const uint8_t *plain, size_t plen, uint8_t **out, size_t *olen){*olen=12+plen+16; *out=(uint8_t*)malloc(*olen); if(!*out)return ESP_ERR_NO_MEM; uint8_t *iv=*out,*cipher=*out+12,*tag=*out+12+plen; esp_fill_random(iv,12); mbedtls_gcm_context gc; mbedtls_gcm_init(&gc); mbedtls_gcm_setkey(&gc,MBEDTLS_CIPHER_ID_AES,s_key,256); mbedtls_gcm_crypt_and_tag(&gc,MBEDTLS_GCM_ENCRYPT,plen,iv,12,NULL,0,plain,cipher,16,tag); mbedtls_gcm_free(&gc); return ESP_OK;}
static esp_err_t decrypt_blob(const uint8_t *in, size_t ilen, uint8_t *plain, size_t plen){if(ilen<28)return ESP_ERR_INVALID_SIZE; const uint8_t *iv=in,*cipher=in+12,*tag=in+12+plen; mbedtls_gcm_context gc; mbedtls_gcm_init(&gc); mbedtls_gcm_setkey(&gc,MBEDTLS_CIPHER_ID_AES,s_key,256); int r=mbedtls_gcm_auth_decrypt(&gc,plen,iv,12,NULL,0,tag,16,cipher,plain); mbedtls_gcm_free(&gc); return r==0?ESP_OK:ESP_ERR_INVALID_CRC;}
static esp_err_t nvs_save_enc(const char *key,const void *d,size_t sz){ if(!s_ready){nvs_handle_t h; nvs_open(NVS_NS,NVS_READWRITE,&h); nvs_set_blob(h,key,d,sz); nvs_commit(h); nvs_close(h); return ESP_OK;} uint8_t *enc; size_t elen; esp_err_t e=encrypt_blob((const uint8_t*)d,sz,&enc,&elen); if(e)return e; nvs_handle_t h; nvs_open(NVS_NS,NVS_READWRITE,&h); nvs_set_blob(h,key,enc,elen); nvs_commit(h); nvs_close(h); free(enc); return ESP_OK; }
static esp_err_t nvs_load_enc(const char *key,void *d,size_t sz){ nvs_handle_t h; if(nvs_open(NVS_NS,NVS_READONLY,&h)!=ESP_OK)return ESP_ERR_NOT_FOUND; uint8_t buf[512]; size_t bsz=sizeof(buf); esp_err_t e=nvs_get_blob(h,key,buf,&bsz); nvs_close(h); if(e)return e; if(!s_ready){memcpy(d,buf,sz<bsz?sz:bsz); return ESP_OK;} return decrypt_blob(buf,bsz,(uint8_t*)d,sz);} 
esp_err_t crypto_save_tuya_creds(const tuya_creds_t *c,size_t sz){return nvs_save_enc("tuya",c,sz);} 
esp_err_t crypto_load_tuya_creds(tuya_creds_t *c,size_t sz){return nvs_load_enc("tuya",c,sz);} 
esp_err_t crypto_save_satel_creds(const satel_creds_t *c,size_t sz){return nvs_save_enc("satel",c,sz);} 
esp_err_t crypto_load_satel_creds(satel_creds_t *c,size_t sz){return nvs_load_enc("satel",c,sz);} 
void crypto_erase_secrets(void){nvs_flash_erase(); memset(s_key,0,32); s_ready=false;}
