#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODE_HOME = 0,
    MODE_AWAY = 1,
    MODE_NIGHT = 2,
    MODE_MANUAL = 3
} bridge_mode_t;

typedef struct {
    bool enabled;
    uint8_t hour_from;
    uint8_t minute_from;
    uint8_t hour_to;
    uint8_t minute_to;
    bridge_mode_t only_mode;
    uint32_t delay_ms;
    uint32_t auto_off_ms;
} rule_time_window_t;

typedef struct {
    bridge_mode_t current_mode;
    bool quiet_hours;
} automation_modes_status_t;

esp_err_t automation_modes_init(void);
void automation_modes_set(bridge_mode_t mode);
bridge_mode_t automation_modes_get(void);
bool automation_modes_match(const rule_time_window_t *tw, uint32_t now_epoch);
bool automation_modes_quiet_hours(uint8_t h_from, uint8_t m_from, uint8_t h_to, uint8_t m_to, uint32_t now_epoch);
const automation_modes_status_t *automation_modes_status(void);
char *automation_modes_status_json(void);

#ifdef __cplusplus
}
#endif
