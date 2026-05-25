/**
 * tuya_http.cpp — Niskopoziomowe żądania TUYA OpenAPI v1.0
 * ──────────────────────────────────────────────────────────
 * Podpisywanie żądań:
 *   sign = HMAC-SHA256(
 *     client_id + access_token + timestamp + nonce + str_to_sign,
 *     client_secret
 *   ).toUpperHex()
 *
 *   str_to_sign = "GET\n" + SHA256("") + "\n\n" + "/v1.0/..."
 *
 * Ref: https://developer.tuya.com/en/docs/cloud/signing?id=Kags45je3z0qv
 */

#include "tuya_http.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "TUYA_HTTP";

/* Regiony → base URL */
static const char *region_url(const char *region) {
    if (strcmp(region, "us") == 0) return "https://openapi.tuyaus.com";
    if (strcmp(region, "cn") == 0) return "https://openapi.tuyacn.com";
    if (strcmp(region, "in") == 0) return "https://openapi.tuyain.com";
    return "https://openapi.tuyaeu.com";  /* EU domyślnie */
}

/* ── Hex encode ──────────────────────────────────────────────────────────── */
static void to_hex(const uint8_t *in, size_t len, char *out) {
    for (size_t i = 0; i < len; i++)
        sprintf(out + i * 2, "%02x", in[i]);
    out[len * 2] = '\0';
}

/* ── SHA256 ciągu → hex ──────────────────────────────────────────────────── */
static void sha256_hex(const char *data, char out[65]) {
    uint8_t hash[32];
    mbedtls_sha256((const uint8_t*)data, strlen(data), hash, 0);
    to_hex(hash, 32, out);
}

/* ── HMAC-SHA256 → HEX UPPERCASE ─────────────────────────────────────────── */
static esp_err_t hmac_sha256_hex(const char *key, size_t key_len,
                                  const char *data, size_t data_len,
                                  char out[65]) {
    uint8_t mac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int r = mbedtls_md_setup(&ctx,
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    if (r == 0) r = mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, key_len);
    if (r == 0) r = mbedtls_md_hmac_update(&ctx, (const uint8_t*)data, data_len);
    if (r == 0) r = mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);
    if (r != 0) return ESP_FAIL;
    to_hex(mac, 32, out);
    /* Konwertuj na UPPERCASE */
    for (int i = 0; out[i]; i++)
        if (out[i] >= 'a' && out[i] <= 'f') out[i] -= 32;
    return ESP_OK;
}

/* ── Buduj nagłówek sign ─────────────────────────────────────────────────── */
static esp_err_t build_sign(const tuya_http_ctx_t *ctx,
                              const char *method,
                              const char *path,
                              const char *body,
                              char sign_out[65]) {
    /* 1. SHA256 ciała żądania */
    char body_hash[65];
    sha256_hex(body ? body : "", body_hash);

    /* 2. str_to_sign */
    char str_to_sign[512];
    snprintf(str_to_sign, sizeof(str_to_sign),
        "%s\n%s\n\n%s", method, body_hash, path);

    /* 3. Buduj string do podpisu:
          client_id + access_token + timestamp + nonce + str_to_sign */
    char to_sign[1024];
    snprintf(to_sign, sizeof(to_sign),
        "%s%s%lld%s%s",
        ctx->client_id,
        ctx->access_token[0] ? ctx->access_token : "",
        (long long)ctx->timestamp_ms,
        ctx->nonce,
        str_to_sign);

    /* 4. HMAC podpis */
    return hmac_sha256_hex(ctx->client_secret, strlen(ctx->client_secret),
                            to_sign, strlen(to_sign), sign_out);
}

/* ── Wspólna obsługa odpowiedzi HTTP ─────────────────────────────────────── */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t http_event(esp_http_client_event_t *evt) {
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb) {
        size_t new_len = rb->len + evt->data_len;
        if (new_len + 1 > rb->cap) {
            rb->cap = new_len + 1024;
            rb->buf = (char *)realloc(rb->buf, rb->cap);
        }
        memcpy(rb->buf + rb->len, evt->data, evt->data_len);
        rb->len = new_len;
        rb->buf[rb->len] = '\0';
    }
    return ESP_OK;
}

/* ── Główna funkcja żądania ──────────────────────────────────────────────── */
esp_err_t tuya_http_request(tuya_http_ctx_t *ctx,
                              const char *method,
                              const char *path,
                              const char *body_json,
                              char **resp_out) {
    /* Timestamp w ms */
    ctx->timestamp_ms = (int64_t)time(NULL) * 1000LL;
    /* Nonce: 8-znakowy hex z esp_random */
    uint32_t rnd = esp_random();
    snprintf(ctx->nonce, sizeof(ctx->nonce), "%08lx", (unsigned long)rnd);

    /* Podpis */
    char sign[65];
    esp_err_t err = build_sign(ctx, method, path, body_json, sign);
    if (err != ESP_OK) return err;

    /* Zbuduj URL */
    char url[256];
    snprintf(url, sizeof(url), "%s%s", region_url(ctx->region), path);

    /* Klient HTTP */
    resp_buf_t rb = {.buf=(char*)calloc(1,2048), .len=0, .cap=2048};
    esp_http_client_config_t cfg = {
        .url    = url,
        .method = (strcmp(method, "POST") == 0)
                    ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms          = 8000,
        .transport_type      = HTTP_TRANSPORT_OVER_SSL,
        .skip_cert_common_name_check = true,
        .event_handler       = http_event,
        .user_data           = &rb,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    /* Nagłówki TUYA */
    char ts_str[24];
    snprintf(ts_str, sizeof(ts_str), "%lld", (long long)ctx->timestamp_ms);

    esp_http_client_set_header(client, "client_id",    ctx->client_id);
    esp_http_client_set_header(client, "sign",         sign);
    esp_http_client_set_header(client, "t",            ts_str);
    esp_http_client_set_header(client, "sign_method",  "HMAC-SHA256");
    esp_http_client_set_header(client, "nonce",        ctx->nonce);
    if (ctx->access_token[0])
        esp_http_client_set_header(client, "access_token", ctx->access_token);

    if (body_json) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body_json, strlen(body_json));
    }

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGE(TAG, "%s %s → HTTP %d err=0x%x", method, path, status, err);
        free(rb.buf);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    ESP_LOGD(TAG, "%s %s → %d (%u B)", method, path, status, (unsigned)rb.len);
    *resp_out = rb.buf;
    return ESP_OK;
}
