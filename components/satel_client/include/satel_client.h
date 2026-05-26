#pragma once
#include "esp_err.h"
#include <stdbool.h>
typedef enum{SATEL_STATE_IDLE=0,SATEL_STATE_CONNECTING,SATEL_STATE_READY,SATEL_STATE_ERROR,SATEL_STATE_RECONNECT}satel_state_t;
esp_err_t satel_client_init(void);
esp_err_t satel_client_start(void);
esp_err_t satel_client_stop(void);
esp_err_t satel_client_test(void);
satel_state_t satel_client_get_state(void);
int satel_get_violated_count(void);
bool satel_is_zone_violated(uint8_t zone);
esp_err_t satel_output_on(uint8_t output);
esp_err_t satel_output_off(uint8_t output);
esp_err_t satel_ip_input_on(uint8_t input_num);
esp_err_t satel_ip_input_off(uint8_t input_num);
char *satel_state_to_json(void);
