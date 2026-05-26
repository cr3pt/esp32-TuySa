#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char key[32];
    uint32_t window_start;
    uint16_t count;
    uint16_t limit;
    uint32_t window_s;
} rate_limit_entry_t;

void rate_limit_init(void);
bool rate_limit_allow(const char *key, uint16_t limit, uint32_t window_s);
char *rate_limit_status_json(void);

#ifdef __cplusplus
}
#endif
