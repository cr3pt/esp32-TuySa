#include "automation_modes.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static automation_modes_status_t s_status = { MODE_HOME, false };

static void epoch_to_hm(uint32_t epoch, int *h, int *m) {
    time_t t = (time_t)epoch;
    struct tm tmv = {};
    localtime_r(&t, &tmv);
    *h = tmv.tm_hour;
    *m = tmv.tm_min;
}

esp_err_t automation_modes_init(void) {
    s_status.current_mode = MODE_HOME;
    s_status.quiet_hours = false;
    return ESP_OK;
}

void automation_modes_set(bridge_mode_t mode) { s_status.current_mode = mode; }
bridge_mode_t automation_modes_get(void) { return s_status.current_mode; }

bool automation_modes_quiet_hours(uint8_t h_from, uint8_t m_from, uint8_t h_to, uint8_t m_to, uint32_t now_epoch) {
    int h=0,m=0; epoch_to_hm(now_epoch, &h, &m);
    int now = h*60 + m;
    int from = h_from*60 + m_from;
    int to   = h_to*60 + m_to;
    if (from <= to) return now >= from && now <= to;
    return now >= from || now <= to;
}

bool automation_modes_match(const rule_time_window_t *tw, uint32_t now_epoch) {
    if (!tw || !tw->enabled) return true;
    if (tw->only_mode != s_status.current_mode) return false;
    return automation_modes_quiet_hours(tw->hour_from, tw->minute_from, tw->hour_to, tw->minute_to, now_epoch);
}

const automation_modes_status_t *automation_modes_status(void) { return &s_status; }
char *automation_modes_status_json(void) {
    char *buf = (char*)malloc(128);
    if (!buf) return NULL;
    snprintf(buf, 128, "{\"mode\":%d,\"quiet_hours\":%s}", (int)s_status.current_mode, s_status.quiet_hours?"true":"false");
    return buf;
}
