#include "event_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static event_entry_t s_log[EVENT_LOG_MAX];
static int s_head = 0;
static int s_count = 0;

esp_err_t event_log_init(void) {
    memset(s_log, 0, sizeof(s_log));
    s_head = 0;
    s_count = 0;
    return ESP_OK;
}

void event_log_add(event_level_t level, const char *source, const char *fmt, ...) {
    event_entry_t *e = &s_log[s_head];
    memset(e, 0, sizeof(*e));
    e->ts = (uint32_t)time(NULL);
    e->level = level;
    snprintf(e->source, sizeof(e->source), "%s", source ? source : "sys");
    va_list ap; va_start(ap, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
    s_head = (s_head + 1) % EVENT_LOG_MAX;
    if (s_count < EVENT_LOG_MAX) s_count++;
}

int event_log_count(void) { return s_count; }

int event_log_get(int idx, event_entry_t *out) {
    if (idx < 0 || idx >= s_count || !out) return -1;
    int start = (s_head - s_count + EVENT_LOG_MAX) % EVENT_LOG_MAX;
    int pos = (start + idx) % EVENT_LOG_MAX;
    *out = s_log[pos];
    return 0;
}

char *event_log_to_json(void) {
    char *buf = (char*)malloc(8192);
    if (!buf) return NULL;
    size_t p = 0;
    p += snprintf(buf + p, 8192 - p, "{\"count\":%d,\"events\":[", s_count);
    for (int i = 0; i < s_count; i++) {
        event_entry_t e = {};
        event_log_get(i, &e);
        if (i) p += snprintf(buf + p, 8192 - p, ",");
        p += snprintf(buf + p, 8192 - p,
            "{\"ts\":%lu,\"level\":%d,\"source\":\"%s\",\"message\":\"%s\"}",
            (unsigned long)e.ts, (int)e.level, e.source, e.message);
        if (p > 7800) break;
    }
    p += snprintf(buf + p, 8192 - p, "]}");
    return buf;
}
