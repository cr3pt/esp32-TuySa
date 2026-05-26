#include "tuya_http.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "TUYA_HTTP";

static char s_region[8]  = "eu";
static char s_cid[48]    = {};
static char s_secret[48] = {};

/* Bufor odbiorczy */
static char   s_rbuf[8192];
static size_t s_rlen = 0;

void tuya_http_set_credentials(const char *region,
                                const char *client_id,
                                const char *client_secret) {
    if (region)      strncpy(s_region, region,      sizeof(s_region)-1);
    if (client_id)   strncpy(s_cid,    client_id,   sizeof(s_cid)-1);
    if (client_secret) strncpy(s_secret, client_secret, sizeof(s_secret)-1);
}

/* --- Narzedzia kryptograficzne --- */

static void sha256_hex(const char *s, size_t slen, char *out64) {
    uint8_t h[32];
    mbedtls_sha256((const unsigned char*)s, slen, h, 0);
    for (int i = 0; i < 32; i++) sprintf(out64 + i*2, "%02x", (unsigned)h[i]);
    out64[64] = '\0';
}

static void hmac_sha256_hex(const char *key, size_t klen,
                             const char *msg, size_t mlen,
                             char *out64) {
    uint8_t mac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, md, 1);
    mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, klen);
    mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg, mlen);
    mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);
    for (int i = 0; i < 32; i++) sprintf(out64 + i*2, "%02X", (unsigned)mac[i]);
    out64[64] = '\0';
}

/*
 * Sygnatura TUYA OpenAPI v2.0:
 *   str_to_sign = ClientId + AccessToken + t + nonce +
 *                 METHOD + "\n" + body_sha256 + "\n" + "" + "\n" + path
 *   sign = HMAC-SHA256(client_secret, str_to_sign).toUpperCase()
 */
static void build_sign(const char *token,
                        const char *method,
                        const char *path,
                        const char *body,
                        char *sign_out,   /* 65 */
                        char *ts_out,     /* 16 */
                        char *nonce_out   /* 36 */) {
    long long ts = (long long)time(NULL) * 1000LL;
    snprintf(ts_out,    16,  "%lld", ts);
    snprintf(nonce_out, 36,  "%08llx%08llx",
             (unsigned long long)(ts & 0xFFFFFFFFULL),
             (unsigned long long)(ts >> 32));

    char body_hash[65];
    sha256_hex(body && *body ? body : "", body && *body ? strlen(body) : 0, body_hash);

    /* headers_str = pusty string (nie wysylamy custom headers do sygnatury) */
    char str_to_sign[1024];
    int n = snprintf(str_to_sign, sizeof(str_to_sign),
                     "%s%s%s%s%s\n%s\n\n%s",
                     s_cid,
                     token  ? token  : "",
                     ts_out,
                     nonce_out,
                     method,
                     body_hash,
                     path);
    if (n < 0 || n >= (int)sizeof(str_to_sign))
        str_to_sign[sizeof(str_to_sign)-1] = '\0';

    hmac_sha256_hex(s_secret, strlen(s_secret),
                    str_to_sign, strlen(str_to_sign),
                    sign_out);
}

/* --- Callback HTTP --- */
static esp_err_t http_event_cb(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        size_t avail = sizeof(s_rbuf) - s_rlen - 1;
        size_t copy  = e->data_len < avail ? e->data_len : avail;
        memcpy(s_rbuf + s_rlen, e->data, copy);
        s_rlen += copy;
    }
    if (e->event_id == HTTP_EVENT_ON_FINISH || e->event_id == HTTP_EVENT_DISCONNECTED)
        s_rbuf[s_rlen] = '\0';
    return ESP_OK;
}

/* --- Wspolny request --- */
static esp_err_t do_request(const char *method,
                              const char *path,
                              const char *token,
                              const char *body,
                              tuya_resp_t *out) {
    char url[256];
    snprintf(url, sizeof(url), "https://openapi.tuya%s.com%s", s_region, path);

    char sign[65]={}, ts[16]={}, nonce[36]={};
    build_sign(token, method, path, body, sign, ts, nonce);

    esp_http_client_config_t cfg = {};
    cfg.url                = url;
    cfg.event_handler      = http_event_cb;
    cfg.transport_type     = HTTP_TRANSPORT_OVER_SSL;
    cfg.crt_bundle_attach  = esp_crt_bundle_attach;
    cfg.timeout_ms         = 10000;
    cfg.buffer_size        = 4096;

    s_rlen = 0; memset(s_rbuf, 0, sizeof(s_rbuf));

    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(h, "client_id",   s_cid);
    esp_http_client_set_header(h, "sign",         sign);
    esp_http_client_set_header(h, "t",            ts);
    esp_http_client_set_header(h, "nonce",        nonce);
    esp_http_client_set_header(h, "sign_method",  "HMAC-SHA256");
    esp_http_client_set_header(h, "Content-Type", "application/json");
    if (token && *token)
        esp_http_client_set_header(h, "access_token", token);

    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(h, HTTP_METHOD_POST);
        if (body && *body)
            esp_http_client_set_post_field(h, body, strlen(body));
    } else {
        esp_http_client_set_method(h, HTTP_METHOD_GET);
    }

    esp_err_t e = esp_http_client_perform(h);
    out->status   = esp_http_client_get_status_code(h);
    out->body     = strndup(s_rbuf, s_rlen);
    out->body_len = s_rlen;
    esp_http_client_cleanup(h);

    if (e != ESP_OK)
        ESP_LOGE(TAG, "%s %s -> err %d", method, path, e);
    else
        ESP_LOGD(TAG, "%s %s -> %d", method, path, out->status);

    return e;
}

/* --- API publiczne --- */
esp_err_t tuya_http_get(const char *path, const char *token, tuya_resp_t *out) {
    return do_request("GET", path, token, NULL, out);
}
esp_err_t tuya_http_post(const char *path, const char *token,
                          const char *body, tuya_resp_t *out) {
    return do_request("POST", path, token, body, out);
}
void tuya_resp_free(tuya_resp_t *r) { if (r && r->body) { free(r->body); r->body=NULL; r->body_len=0; } }

/* --- Token --- */
static bool jstr(const char *json, const char *key, char *out, size_t olen) {
    char needle[64]; snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle); if (!p) return false;
    p += strlen(needle);
    const char *e = strchr(p, '"'); if (!e) return false;
    size_t l = (size_t)(e-p); if (l >= olen) l = olen-1;
    memcpy(out, p, l); out[l] = '\0'; return true;
}

esp_err_t tuya_http_get_token(char *token_out, size_t token_len,
                               uint32_t *expire_out) {
    tuya_resp_t r = {};
    esp_err_t e = tuya_http_get("/v1.0/token?grant_type=1", NULL, &r);
    if (e != ESP_OK || !r.body) { tuya_resp_free(&r); return ESP_FAIL; }
    bool ok = jstr(r.body, "access_token", token_out, token_len);
    if (expire_out) {
        /* "expire_time":7200 */
        const char *p = strstr(r.body, "\"expire_time\":");
        *expire_out = p ? (uint32_t)atoi(p + 14) : 7200;
    }
    tuya_resp_free(&r);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t tuya_http_refresh_token(const char *refresh_token,
                                   char *token_out, size_t token_len) {
    char path[128];
    snprintf(path, sizeof(path), "/v1.0/token/%s", refresh_token);
    tuya_resp_t r = {};
    esp_err_t e = tuya_http_get(path, NULL, &r);
    if (e != ESP_OK || !r.body) { tuya_resp_free(&r); return ESP_FAIL; }
    bool ok = jstr(r.body, "access_token", token_out, token_len);
    tuya_resp_free(&r);
    return ok ? ESP_OK : ESP_FAIL;
}
