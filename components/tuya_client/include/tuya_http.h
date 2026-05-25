#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    client_id[48];
    char    client_secret[48];
    char    user_uid[48];
    char    region[8];
    char    access_token[128];
    char    refresh_token[128];
    int64_t token_expire_s;   /* czas unixa wygaśnięcia tokenu */
    int64_t timestamp_ms;
    char    nonce[16];
} tuya_http_ctx_t;

/**
 * @brief Wykonaj żądanie HTTP do TUYA OpenAPI z podpisem HMAC-SHA256.
 *
 * @param ctx       Kontekst z danymi dostępowymi i tokenem
 * @param method    "GET" lub "POST"
 * @param path      Ścieżka, np. "/v1.0/token?grant_type=1"
 * @param body_json Ciało JSON lub NULL dla GET
 * @param resp_out  [out] Zaalokowana odpowiedź — caller musi free()
 */
esp_err_t tuya_http_request(tuya_http_ctx_t *ctx,
                              const char *method,
                              const char *path,
                              const char *body_json,
                              char **resp_out);

#ifdef __cplusplus
}
#endif
