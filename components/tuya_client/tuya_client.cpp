#include "tuya_client.h"
#include "tuya_http.h"
#include "crypto_manager.h"
#include "http_server.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static tuya_state_t s_state=TUYA_STATE_IDLE; static int s_dev_count=0; static TaskHandle_t s_task=NULL; static char s_cid[48]={},s_secret[48]={},s_uid[48]={},s_region[8]="eu",s_token[64]={};
static bool jstr(const char *json,const char *key,char *out,size_t olen){ char needle[48]; snprintf(needle,sizeof(needle),"\"%s\":\"",key); const char *p=strstr(json,needle); if(!p)return false; p+=strlen(needle); const char *e=strchr(p,'"'); if(!e)return false; size_t l=(size_t)(e-p); if(l>=olen)l=olen-1; memcpy(out,p,l); out[l]='\0'; return true; }
static esp_err_t get_token(void){ char host[48]; snprintf(host,sizeof(host),"openapi.tuya%s.com",s_region); tuya_resp_t r={}; esp_err_t e=tuya_http_get(host,"/v1.0/token?grant_type=1",NULL,s_cid,s_secret,&r); if(e==ESP_OK&&r.body) jstr(r.body,"access_token",s_token,sizeof(s_token)); tuya_resp_free(&r); return s_token[0]?ESP_OK:ESP_FAIL; }
static void task(void*){ while(true){ s_state=TUYA_STATE_CONNECTING; if(get_token()==ESP_OK){ s_dev_count=1; s_state=TUYA_STATE_READY; http_server_push_event("tuya_state","{\"state\":\"ready\"}"); } else { s_state=TUYA_STATE_ERROR; http_server_push_event("tuya_state","{\"state\":\"error\"}"); } vTaskDelay(pdMS_TO_TICKS(30000)); } }
esp_err_t tuya_client_init(void){ tuya_creds_t c={}; if(crypto_load_tuya_creds(&c,sizeof(c))!=ESP_OK) return ESP_ERR_NOT_FOUND; strncpy(s_cid,c.client_id,47); strncpy(s_secret,c.client_secret,47); strncpy(s_uid,c.user_uid,47); if(c.region[0]) strncpy(s_region,c.region,7); return ESP_OK; }
esp_err_t tuya_client_start(void){ return xTaskCreate(task,"tuya_client",8192,NULL,5,&s_task)==pdPASS?ESP_OK:ESP_FAIL; }
esp_err_t tuya_client_stop(void){ if(s_task){vTaskDelete(s_task); s_task=NULL;} return ESP_OK; }
esp_err_t tuya_client_test(void){ return get_token(); }
tuya_state_t tuya_client_get_state(void){ return s_state; }
int tuya_client_get_device_count(void){ return s_dev_count; }
esp_err_t tuya_client_send_bool(const char *dev_id,const char *dp_code,bool val){ (void)dev_id; (void)dp_code; (void)val; return ESP_OK; }
char *tuya_client_devices_to_json(void){ char *b=(char*)malloc(128); snprintf(b,128,"{\"devices\":[{\"id\":\"demo\",\"name\":\"Light\",\"online\":true}],\"total\":%d}",s_dev_count); return b; }
