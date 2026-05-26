#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    int sent_ok;
    int sent_fail;
    char last_url[192];
} webhook_status_t;

esp_err_t webhook_client_init(const char *base_url);
esp_err_t webhook_client_post_json(const char *path, const char *json);
const webhook_status_t *webhook_client_get_status(void);
char *webhook_client_status_json(void);

#ifdef __cplusplus
}
#endif
