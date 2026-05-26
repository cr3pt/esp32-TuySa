#include "watchdog.h"
#include "wifi_manager.h"
#include "tuya_client.h"
#include "satel_client.h"
#include "rule_engine.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <stdio.h>
static TaskHandle_t s_task=NULL; static sys_state_t s_state=SYS_BOOT; static unsigned long s_uptime=0;
static void leds(int g,int y,int r){ gpio_set_level((gpio_num_t)2,g); gpio_set_level((gpio_num_t)4,y); gpio_set_level((gpio_num_t)5,r); }
static void task(void*){ int phase=0; while(true){ s_uptime++; bool wifi=wifi_manager_is_connected(); bool tuya=tuya_client_get_state()==TUYA_STATE_READY; bool satel=satel_client_get_state()==SATEL_STATE_READY; if(!wifi) s_state=SYS_WIFI_DOWN; else if(!tuya&&!satel) s_state=SYS_BOTH_RECONNECT; else if(!tuya) s_state=SYS_TUYA_RECONNECT; else if(!satel) s_state=SYS_SATEL_RECONNECT; else s_state=SYS_ALL_OK; switch(s_state){ case SYS_BOOT: leds(0,phase&1,0); break; case SYS_ALL_OK: leds(1,0,0); break; case SYS_WIFI_DOWN: leds(0,0,phase&1); break; case SYS_TUYA_RECONNECT: leds(0,phase&1,0); break; case SYS_SATEL_RECONNECT: leds(0,phase&1,phase&1); break; case SYS_BOTH_RECONNECT: leds(0,phase&1,(phase+1)&1); break; default: leds(0,0,1); break; } phase++; vTaskDelay(pdMS_TO_TICKS(500)); } }
esp_err_t watchdog_init(void){ gpio_reset_pin((gpio_num_t)2); gpio_reset_pin((gpio_num_t)4); gpio_reset_pin((gpio_num_t)5); gpio_set_direction((gpio_num_t)2,GPIO_MODE_OUTPUT); gpio_set_direction((gpio_num_t)4,GPIO_MODE_OUTPUT); gpio_set_direction((gpio_num_t)5,GPIO_MODE_OUTPUT); return ESP_OK; }
esp_err_t watchdog_start(void){ return xTaskCreate(task,"wdog",4096,NULL,8,&s_task)==pdPASS?ESP_OK:ESP_FAIL; }
void watchdog_stop(void){ if(s_task){vTaskDelete(s_task); s_task=NULL;} }
char *watchdog_health_to_json(void){ char ip[20]="0.0.0.0"; wifi_manager_get_ip_str(ip,sizeof(ip)); char *b=(char*)malloc(512); snprintf(b,512,"{\"state\":\"%s\",\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d},\"tuya\":{\"connected\":%s,\"devices\":%d,\"reconnects\":0},\"satel\":{\"connected\":%s,\"panel\":\"SATEL INTEGRA\",\"violated_zones\":%d,\"armed_parts\":0,\"reconnects\":0},\"rules\":{\"total\":%d,\"enabled\":%d,\"fired\":0},\"system\":{\"uptime\":%lu,\"free_heap\":0,\"min_heap\":0}}", s_state==SYS_ALL_OK?"ok":"degraded", wifi_manager_is_connected()?"true":"false", ip, wifi_manager_get_rssi(), tuya_client_get_state()==TUYA_STATE_READY?"true":"false", tuya_client_get_device_count(), satel_client_get_state()==SATEL_STATE_READY?"true":"false", satel_get_violated_count(), rule_engine_count(), rule_engine_count(), s_uptime); return b; }
