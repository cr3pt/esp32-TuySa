#include "wifi_manager.h"
#include "config_manager.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
static EventGroupHandle_t s_eg=NULL; static wifi_state_cb_t s_cb=NULL; static bool s_connected=false; static uint32_t s_ip=0; 
#define WIFI_CONNECTED_BIT BIT0
static void wifi_event_handler(void*,esp_event_base_t eb,int32_t id,void*d){ if(eb==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED){s_connected=false; if(s_cb)s_cb(WIFI_STATE_DISCONNECTED,0); esp_wifi_connect();} else if(eb==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){ip_event_got_ip_t *e=(ip_event_got_ip_t*)d; s_ip=e->ip_info.ip.addr; s_connected=true; if(s_eg)xEventGroupSetBits(s_eg,WIFI_CONNECTED_BIT); if(s_cb)s_cb(WIFI_STATE_CONNECTED,s_ip);} }
esp_err_t wifi_manager_init(void){ s_eg=xEventGroupCreate(); ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default()); esp_netif_create_default_wifi_sta(); wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&cfg)); esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event_handler,NULL,NULL); esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event_handler,NULL,NULL); return ESP_OK; }
esp_err_t wifi_manager_start_sta(uint32_t timeout_ms){ net_config_t net={}; config_manager_load_net(&net); wifi_config_t wc={}; strncpy((char*)wc.sta.ssid,net.ssid,31); strncpy((char*)wc.sta.password,net.password,63); ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&wc)); ESP_ERROR_CHECK(esp_wifi_start()); esp_wifi_connect(); EventBits_t bits=xEventGroupWaitBits(s_eg,WIFI_CONNECTED_BIT,pdFALSE,pdFALSE,pdMS_TO_TICKS(timeout_ms)); return (bits&WIFI_CONNECTED_BIT)?ESP_OK:ESP_ERR_TIMEOUT; }
esp_err_t wifi_manager_start_ap(const char *ssid){ esp_netif_create_default_wifi_ap(); wifi_config_t wc={}; strncpy((char*)wc.ap.ssid,ssid,31); wc.ap.authmode=WIFI_AUTH_OPEN; wc.ap.max_connection=4; ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP)); ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&wc)); return esp_wifi_start(); }
bool wifi_manager_is_connected(void){return s_connected;} int8_t wifi_manager_get_rssi(void){wifi_ap_record_t r={};esp_wifi_sta_get_ap_info(&r);return r.rssi;} void wifi_manager_get_ip_str(char *b,size_t l){snprintf(b,l,"%lu.%lu.%lu.%lu",s_ip&0xFF,(s_ip>>8)&0xFF,(s_ip>>16)&0xFF,(s_ip>>24)&0xFF);} void wifi_manager_set_state_cb(wifi_state_cb_t cb){s_cb=cb;}
