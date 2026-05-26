#pragma once
#include "esp_err.h"
#include "satel_protocol.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SATEL_STATE_IDLE        = 0,
    SATEL_STATE_CONNECTING  = 1,
    SATEL_STATE_READY       = 2,
    SATEL_STATE_POLLING     = 3,
    SATEL_STATE_ERROR       = 4,
    SATEL_STATE_RECONNECT   = 5,
} satel_state_t;

/* Callback wywoływany przy zmianie stanu dowolnej strefy/wyjscia */
typedef void (*satel_zone_cb_t)(int zone_num, bool violated, void *user);
typedef void (*satel_output_cb_t)(int out_num, bool active, void *user);
typedef void (*satel_state_change_cb_t)(satel_state_t state, void *user);

esp_err_t satel_client_init(void);
esp_err_t satel_client_start(void);
esp_err_t satel_client_stop(void);
esp_err_t satel_client_test(void);

satel_state_t satel_client_get_state(void);
int           satel_client_get_reconnect_count(void);
const satel_panel_state_t *satel_client_get_panel_state(void);

bool satel_is_zone_violated(uint8_t zone);
bool satel_is_zone_alarmed(uint8_t zone);
bool satel_is_output_active(uint8_t output);
bool satel_is_partition_armed(uint8_t part);
int  satel_get_violated_count(void);

/* Komendy sterujace */
esp_err_t satel_output_on(uint8_t output);
esp_err_t satel_output_off(uint8_t output);
esp_err_t satel_arm(uint8_t partition, uint8_t mode);
esp_err_t satel_disarm(uint8_t partition);
esp_err_t satel_clear_alarm(uint8_t partition);

/* Wejscia IP modulu ETHM-1 PLUS (HTTP GET do modulu) */
esp_err_t satel_ip_input_on(uint8_t input_num);
esp_err_t satel_ip_input_off(uint8_t input_num);

/* Callbacki */
void satel_set_zone_cb(satel_zone_cb_t cb, void *user);
void satel_set_output_cb(satel_output_cb_t cb, void *user);
void satel_set_state_cb(satel_state_change_cb_t cb, void *user);

/* Serializacja stanu do JSON */
char *satel_state_to_json(void);

#ifdef __cplusplus
}
#endif
