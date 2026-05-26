#pragma once
#include "esp_err.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_LOG_MAX 64
#define EVENT_LOG_MSG 128

typedef enum {
    EV_INFO = 0,
    EV_WARN = 1,
    EV_ERROR = 2,
    EV_SATEL = 3,
    EV_TUYA = 4,
    EV_MQTT = 5,
    EV_WEBHOOK = 6
} event_level_t;

typedef struct {
    uint32_t ts;
    event_level_t level;
    char source[16];
    char message[EVENT_LOG_MSG];
} event_entry_t;

esp_err_t event_log_init(void);
void event_log_add(event_level_t level, const char *source, const char *fmt, ...);
int event_log_count(void);
int event_log_get(int idx, event_entry_t *out);
char *event_log_to_json(void);

#ifdef __cplusplus
}
#endif
