#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool running;
    bool auth_enabled;
    char username[32];
} http_server_status_t;

esp_err_t http_server_init(const char *username, const char *password);
esp_err_t http_server_start(void);
esp_err_t http_server_stop(void);
void      http_server_push_event(const char *event, const char *data);
const http_server_status_t *http_server_get_status(void);
char     *http_server_status_json(void);

#ifdef __cplusplus
}
#endif
