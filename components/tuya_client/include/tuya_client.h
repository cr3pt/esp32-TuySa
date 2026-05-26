#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define TUYA_MAX_DEVICES   32
#define TUYA_MAX_DP        16
#define TUYA_DEV_ID_LEN    32
#define TUYA_DEV_NAME_LEN  48

typedef enum {
    TUYA_STATE_IDLE       = 0,
    TUYA_STATE_CONNECTING = 1,
    TUYA_STATE_READY      = 2,
    TUYA_STATE_POLLING    = 3,
    TUYA_STATE_TOKEN_REFRESH = 4,
    TUYA_STATE_ERROR      = 5,
    TUYA_STATE_RECONNECT  = 6,
} tuya_state_t;

typedef enum {
    TUYA_DP_BOOL   = 0,
    TUYA_DP_INT    = 1,
    TUYA_DP_STRING = 2,
    TUYA_DP_ENUM   = 3,
    TUYA_DP_RAW    = 4,
} tuya_dp_type_t;

typedef struct {
    char           code[32];
    tuya_dp_type_t type;
    union {
        bool    b;
        int32_t i;
        char    s[32];
    } value;
} tuya_dp_t;

typedef struct {
    char     id[TUYA_DEV_ID_LEN];
    char     name[TUYA_DEV_NAME_LEN];
    char     category[16];
    bool     online;
    uint32_t last_seen;
    tuya_dp_t dp[TUYA_MAX_DP];
    int       dp_count;
} tuya_device_t;

/* Callback przy zmianie DP dowolnego urzadzenia */
typedef void (*tuya_dp_change_cb_t)(const char *dev_id,
                                     const tuya_dp_t *dp,
                                     void *user);
typedef void (*tuya_state_cb_t)(tuya_state_t state, void *user);

esp_err_t tuya_client_init(void);
esp_err_t tuya_client_start(void);
esp_err_t tuya_client_stop(void);
esp_err_t tuya_client_test(void);   /* Proba pobrania tokena */

tuya_state_t tuya_client_get_state(void);
int          tuya_client_get_reconnect_count(void);
int          tuya_client_get_device_count(void);

/* Dostep do urzadzen (thread-safe kopia) */
bool tuya_client_get_device(int idx, tuya_device_t *out);
bool tuya_client_find_device(const char *id, tuya_device_t *out);
bool tuya_client_get_dp(const char *dev_id, const char *dp_code,
                         tuya_dp_t *out);

/* Sterowanie */
esp_err_t tuya_cmd_bool(const char *dev_id, const char *dp_code, bool val);
esp_err_t tuya_cmd_int(const char *dev_id, const char *dp_code, int32_t val);
esp_err_t tuya_cmd_string(const char *dev_id, const char *dp_code,
                           const char *val);

/* Callbacki */
void tuya_set_dp_change_cb(tuya_dp_change_cb_t cb, void *user);
void tuya_set_state_cb(tuya_state_cb_t cb, void *user);

/* JSON */
char *tuya_devices_to_json(void);
char *tuya_status_to_json(void);

#ifdef __cplusplus
}
#endif
