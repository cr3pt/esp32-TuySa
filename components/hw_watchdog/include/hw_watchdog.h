#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hw_watchdog_init(int timeout_s);
esp_err_t hw_watchdog_add_current_task(void);
esp_err_t hw_watchdog_feed(void);
char *hw_watchdog_status_json(void);

#ifdef __cplusplus
}
#endif
