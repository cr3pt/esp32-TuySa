#include "rate_limit.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define RL_MAX 16
static rate_limit_entry_t s_rl[RL_MAX];

void rate_limit_init(void) { memset(s_rl, 0, sizeof(s_rl)); }

bool rate_limit_allow(const char *key, uint16_t limit, uint32_t window_s) {
    uint32_t now = (uint32_t)time(NULL);
    int free_idx = -1;
    for (int i = 0; i < RL_MAX; i++) {
        if (!s_rl[i].key[0] && free_idx < 0) free_idx = i;
        if (strcmp(s_rl[i].key, key ? key : "") == 0) {
            if (now - s_rl[i].window_start >= s_rl[i].window_s) {
                s_rl[i].window_start = now;
                s_rl[i].count = 1;
                s_rl[i].limit = limit;
                s_rl[i].window_s = window_s;
                return true;
            }
            if (s_rl[i].count >= s_rl[i].limit) return false;
            s_rl[i].count++;
            return true;
        }
    }
    if (free_idx >= 0) {
        snprintf(s_rl[free_idx].key, sizeof(s_rl[free_idx].key), "%s", key ? key : "");
        s_rl[free_idx].window_start = now;
        s_rl[free_idx].count = 1;
        s_rl[free_idx].limit = limit;
        s_rl[free_idx].window_s = window_s;
        return true;
    }
    return false;
}

char *rate_limit_status_json(void) {
    char *buf = (char*)malloc(1024);
    if (!buf) return NULL;
    size_t p = 0;
    p += snprintf(buf + p, 1024 - p, "{\"entries\":[");
    int first = 1;
    for (int i = 0; i < RL_MAX; i++) {
        if (!s_rl[i].key[0]) continue;
        if (!first) p += snprintf(buf + p, 1024 - p, ",");
        first = 0;
        p += snprintf(buf + p, 1024 - p,
            "{\"key\":\"%s\",\"count\":%u,\"limit\":%u,\"window_s\":%lu}",
            s_rl[i].key, s_rl[i].count, s_rl[i].limit, (unsigned long)s_rl[i].window_s);
    }
    p += snprintf(buf + p, 1024 - p, "]}");
    return buf;
}
