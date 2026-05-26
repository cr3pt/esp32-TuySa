#pragma once
#include "esp_err.h"
#include <stdbool.h>
typedef enum{TUYA_STATE_IDLE=0,TUYA_STATE_CONNECTING,TUYA_STATE_READY,TUYA_STATE_ERROR,TUYA_STATE_RECONNECT}tuya_state_t;
esp_err_t tuya_client_init(void);
esp_err_t tuya_client_start(void);
esp_err_t tuya_client_stop(void);
esp_err_t tuya_client_test(void);
tuya_state_t tuya_client_get_state(void);
int tuya_client_get_device_count(void);
esp_err_t tuya_client_send_bool(const char *dev_id,const char *dp_code,bool val);
char *tuya_client_devices_to_json(void);
