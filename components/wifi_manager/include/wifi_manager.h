#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
typedef enum{WIFI_STATE_DISCONNECTED=0,WIFI_STATE_CONNECTING,WIFI_STATE_CONNECTED}wifi_state_t;
typedef void(*wifi_state_cb_t)(wifi_state_t,uint32_t ip);
esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start_sta(uint32_t timeout_ms);
esp_err_t wifi_manager_start_ap(const char *ssid);
bool wifi_manager_is_connected(void);
int8_t wifi_manager_get_rssi(void);
void wifi_manager_get_ip_str(char *buf,size_t len);
void wifi_manager_set_state_cb(wifi_state_cb_t cb);
