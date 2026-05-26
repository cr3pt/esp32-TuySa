#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool running;
    bool last_ok;
    int  last_status;
    char last_url[192];
} ota_status_t;

esp_err_t ota_manager_init(void);
esp_err_t ota_manager_https_url(const char *url);
esp_err_t ota_manager_mark_valid(void);
const ota_status_t *ota_manager_get_status(void);
char *ota_manager_status_json(void);

#ifdef __cplusplus
}
#endif
