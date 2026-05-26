#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { SYS_BOOT=0, SYS_ALL_OK, SYS_WIFI_DOWN, SYS_TUYA_RECONNECT, SYS_SATEL_RECONNECT, SYS_BOTH_RECONNECT, SYS_ERROR_CRITICAL } sys_state_t;
esp_err_t watchdog_init(void);
esp_err_t watchdog_start(void);
void watchdog_stop(void);
char *watchdog_health_to_json(void);
#ifdef __cplusplus
}
#endif
