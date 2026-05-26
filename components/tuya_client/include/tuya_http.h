#pragma once
#include "esp_err.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   *body;
    int     status;
    size_t  body_len;
} tuya_resp_t;

/* Inicjalizacja (ustawia cid/secret raz globalnie) */
void tuya_http_set_credentials(const char *region,
                                const char *client_id,
                                const char *client_secret);

/* Pobierz token dostepowy (grant_type=1) */
esp_err_t tuya_http_get_token(char *token_out, size_t token_len,
                               uint32_t *expire_out);

/* Odswiesz token (grant_type=2, wymaga refresh_token) */
esp_err_t tuya_http_refresh_token(const char *refresh_token,
                                   char *token_out, size_t token_len);

/* GET do TUYA Cloud z autoryzacja HMAC-SHA256 */
esp_err_t tuya_http_get(const char *path, const char *token,
                         tuya_resp_t *out);

/* POST do TUYA Cloud */
esp_err_t tuya_http_post(const char *path, const char *token,
                          const char *body, tuya_resp_t *out);

void tuya_resp_free(tuya_resp_t *r);

#ifdef __cplusplus
}
#endif
