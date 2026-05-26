#pragma once
#include "esp_err.h"
typedef struct { char *body; int status; } tuya_resp_t;
esp_err_t tuya_http_get(const char *host,const char *path,const char *token,const char *client_id,const char *secret,tuya_resp_t *out);
esp_err_t tuya_http_post(const char *host,const char *path,const char *token,const char *client_id,const char *secret,const char *body,tuya_resp_t *out);
void tuya_resp_free(tuya_resp_t *r);
