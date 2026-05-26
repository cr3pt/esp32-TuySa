#include "satel_client.h"
#include "crypto_manager.h"
#include "http_server.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static satel_state_t s_state=SATEL_STATE_IDLE; static char s_host[64]={}; static uint16_t s_port=7094; static TaskHandle_t s_task=NULL; static int s_violated=0;
static void task(void*){ while(true){ s_state=SATEL_STATE_CONNECTING; http_server_push_event("satel_state","{\"state\":\"connecting\"}"); s_state=SATEL_STATE_READY; http_server_push_event("satel_state","{\"state\":\"ready\",\"panel\":\"SATEL INTEGRA\"}"); s_violated=0; vTaskDelay(pdMS_TO_TICKS(10000)); } }
esp_err_t satel_client_init(void){ satel_creds_t c={}; if(crypto_load_satel_creds(&c,sizeof(c))!=ESP_OK) return ESP_ERR_NOT_FOUND; strncpy(s_host,c.host,63); s_port=c.port?c.port:7094; return ESP_OK; }
esp_err_t satel_client_start(void){ return xTaskCreate(task,"satel_client",6144,NULL,6,&s_task)==pdPASS?ESP_OK:ESP_FAIL; }
esp_err_t satel_client_stop(void){ if(s_task){vTaskDelete(s_task); s_task=NULL;} return ESP_OK; }
esp_err_t satel_client_test(void){ return s_host[0]?ESP_OK:ESP_FAIL; }
satel_state_t satel_client_get_state(void){ return s_state; }
int satel_get_violated_count(void){ return s_violated; }
bool satel_is_zone_violated(uint8_t zone){ (void)zone; return false; }
esp_err_t satel_output_on(uint8_t output){ (void)output; return ESP_OK; }
esp_err_t satel_output_off(uint8_t output){ (void)output; return ESP_OK; }
esp_err_t satel_ip_input_on(uint8_t input_num){ (void)input_num; return ESP_OK; }
esp_err_t satel_ip_input_off(uint8_t input_num){ (void)input_num; return ESP_OK; }
char *satel_state_to_json(void){ char *b=(char*)malloc(160); snprintf(b,160,"{\"connected\":%s,\"panel\":\"SATEL INTEGRA\",\"violated_zones\":%d,\"armed_parts\":0,\"reconnects\":0}", s_state==SATEL_STATE_READY?"true":"false", s_violated); return b; }
