#include "hw_watchdog.h"
#include "esp_task_wdt.h"
#include <stdlib.h>
#include <stdio.h>

static int s_timeout = 0;
static bool s_started = false;

esp_err_t hw_watchdog_init(int timeout_s) {
    s_timeout = timeout_s;
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms = timeout_s * 1000;
    cfg.idle_core_mask = (1 << 0) | (1 << 1);
    cfg.trigger_panic = true;
    esp_err_t err = esp_task_wdt_init(&cfg);
    if (err == ESP_OK) s_started = true;
    return err;
}

esp_err_t hw_watchdog_add_current_task(void) { return esp_task_wdt_add(NULL); }
esp_err_t hw_watchdog_feed(void) { return esp_task_wdt_reset(); }

char *hw_watchdog_status_json(void) {
    char *buf = (char*)malloc(96);
    if (!buf) return NULL;
    snprintf(buf, 96, "{\"started\":%s,\"timeout_s\":%d}", s_started?"true":"false", s_timeout);
    return buf;
}
