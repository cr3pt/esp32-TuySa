/**
 * tuya_client.cpp — Klient TUYA Cloud OpenAPI v1.0
 * ─────────────────────────────────────────────────
 * Przepływ pracy (task FreeRTOS):
 *
 *  [IDLE]
 *    │  crypto OK + creds loaded
 *    ▼
 *  [CONNECTING] → POST /v1.0/token?grant_type=1 → access_token
 *    │  OK
 *    ▼
 *  [READY]
 *    ├─ Pobierz urządzenia: GET /v1.0/users/{uid}/devices
 *    ├─ Pobierz statusy DP: GET /v1.0/devices/{id}/status
 *    ├─ Poll co 10s: odśwież DP wszystkich urządzeń
 *    ├─ Refresh tokenu co (expire - 120s)
 *    └─ Heartbeat SSE co 30s
 *
 *  [ERROR] → czekaj 10s → [CONNECTING]
 *
 * Parsowanie JSON: własny minimalistyczny parser (bez cJSON)
 * żeby oszczędzić heap (~40KB różnicy).
 */

#include "tuya_client.h"
#include "tuya_http.h"
#include "crypto_manager.h"
#include "http_server.h"
#include "config_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "TUYA";

/* ── Wewnętrzny stan ─────────────────────────────────────────────────────── */
static tuya_http_ctx_t      s_ctx      = {};
static tuya_device_t        s_devs[TUYA_MAX_DEVICES] = {};
static int                  s_dev_cnt  = 0;
static tuya_state_t         s_state    = TUYA_STATE_IDLE;
static SemaphoreHandle_t    s_mutex    = NULL;
static TaskHandle_t         s_task     = NULL;
static tuya_dp_change_cb_t  s_dp_cb    = NULL;

/* Interwały */
#define POLL_INTERVAL_MS    10000
#define HEARTBEAT_MS        30000
#define RECONNECT_DELAY_MS  10000
#define TOKEN_REFRESH_MARGIN_S 120

/* ── Minimalistyczny JSON parser ─────────────────────────────────────────── */
/* Wyciąga wartość stringa dla klucza z płaskiego obiektu JSON */
static bool json_str(const char *json, const char *key,
                     char *out, size_t olen) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return false;
    size_t l = (size_t)(e - p);
    if (l >= olen) l = olen - 1;
    memcpy(out, p, l); out[l] = '\0';
    return true;
}

static bool json_int(const char *json, const char *key, int64_t *out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    *out = strtoll(p, NULL, 10);
    return true;
}

static bool json_bool(const char *json, const char *key, bool *out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    *out = (strncmp(p, "true", 4) == 0);
    return true;
}

/* Sprawdź czy TUYA API zwróciło success */
static bool api_success(const char *resp) {
    bool ok = false;
    json_bool(resp, "success", &ok);
    return ok;
}

/* ── Autentykacja — pobierz token ────────────────────────────────────────── */
static esp_err_t auth_get_token(void) {
    ESP_LOGI(TAG, "Pobieram token TUYA (grant_type=1)…");
    /* Token endpoint nie wymaga access_token w nagłówku */
    s_ctx.access_token[0] = '\0';

    char *resp = NULL;
    esp_err_t err = tuya_http_request(&s_ctx, "GET",
        "/v1.0/token?grant_type=1", NULL, &resp);
    if (err != ESP_OK || !resp) return ESP_FAIL;

    if (!api_success(resp)) {
        ESP_LOGE(TAG, "Token API error: %s", resp);
        free(resp); return ESP_FAIL;
    }

    /* Parsuj token z result */
    const char *result = strstr(resp, "\"result\":");
    if (!result) { free(resp); return ESP_FAIL; }

    json_str(result, "access_token",  s_ctx.access_token,  sizeof(s_ctx.access_token));
    json_str(result, "refresh_token", s_ctx.refresh_token, sizeof(s_ctx.refresh_token));

    int64_t expire = 7200;
    json_int(result, "expire_time", &expire);
    s_ctx.token_expire_s = (int64_t)time(NULL) + expire;

    ESP_LOGI(TAG, "Token OK, wygasa za %lld s", (long long)expire);
    free(resp);
    return ESP_OK;
}

static esp_err_t auth_refresh_token(void) {
    ESP_LOGI(TAG, "Odświeżam token TUYA…");
    char path[128];
    snprintf(path, sizeof(path), "/v1.0/token/%s", s_ctx.refresh_token);
    char *resp = NULL;
    esp_err_t err = tuya_http_request(&s_ctx, "GET", path, NULL, &resp);
    if (err != ESP_OK || !resp) return ESP_FAIL;
    if (!api_success(resp)) { free(resp); return ESP_FAIL; }

    const char *result = strstr(resp, "\"result\":");
    if (result) {
        json_str(result, "access_token",  s_ctx.access_token,  sizeof(s_ctx.access_token));
        json_str(result, "refresh_token", s_ctx.refresh_token, sizeof(s_ctx.refresh_token));
        int64_t expire = 7200;
        json_int(result, "expire_time", &expire);
        s_ctx.token_expire_s = (int64_t)time(NULL) + expire;
        ESP_LOGI(TAG, "Token odświeżony, wygasa za %lld s", (long long)expire);
    }
    free(resp);
    return ESP_OK;
}

/* ── Pobierz listę urządzeń ──────────────────────────────────────────────── */
static esp_err_t fetch_devices(void) {
    ESP_LOGI(TAG, "Pobieram urządzenia dla uid=%s", s_ctx.user_uid);
    char path[128];
    snprintf(path, sizeof(path), "/v1.0/users/%s/devices", s_ctx.user_uid);

    char *resp = NULL;
    esp_err_t err = tuya_http_request(&s_ctx, "GET", path, NULL, &resp);
    if (err != ESP_OK || !resp) return ESP_FAIL;
    if (!api_success(resp)) {
        ESP_LOGE(TAG, "Devices API error: %s", resp);
        free(resp); return ESP_FAIL;
    }

    /* Parsuj tablicę result */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_dev_cnt = 0;
    memset(s_devs, 0, sizeof(s_devs));

    const char *p = strstr(resp, "\"result\":[");
    if (p) p = strchr(p, '[');
    while (p && s_dev_cnt < TUYA_MAX_DEVICES) {
        p = strchr(p, '{');
        if (!p) break;
        /* Znajdź koniec obiektu */
        int depth = 0; const char *q = p;
        while (*q) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }
        /* Skopiuj obiekt do tymczasowego bufora */
        size_t obj_len = (size_t)(q - p);
        char *obj = (char *)malloc(obj_len + 1);
        if (!obj) break;
        memcpy(obj, p, obj_len); obj[obj_len] = '\0';

        tuya_device_t *dev = &s_devs[s_dev_cnt];
        json_str(obj, "id",         dev->id,         sizeof(dev->id));
        json_str(obj, "name",       dev->name,       sizeof(dev->name));
        json_str(obj, "product_id", dev->product_id, sizeof(dev->product_id));
        json_bool(obj, "online", &dev->online);
        if (dev->id[0]) s_dev_cnt++;

        free(obj);
        p = q;
    }
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Pobrano %d urządzeń TUYA", s_dev_cnt);
    free(resp);

    /* SSE push */
    char ev[64];
    snprintf(ev, sizeof(ev), "{\"count\":%d}", s_dev_cnt);
    http_server_push_event("tuya_devices", ev);
    return ESP_OK;
}

/* ── Pobierz statusy DP jednego urządzenia ───────────────────────────────── */
static esp_err_t fetch_device_status(tuya_device_t *dev) {
    char path[96];
    snprintf(path, sizeof(path), "/v1.0/devices/%s/status", dev->id);

    char *resp = NULL;
    esp_err_t err = tuya_http_request(&s_ctx, "GET", path, NULL, &resp);
    if (err != ESP_OK || !resp) return err;
    if (!api_success(resp)) { free(resp); return ESP_FAIL; }

    const char *p = strstr(resp, "\"result\":[");
    dev->dp_count = 0;

    while (p && dev->dp_count < TUYA_MAX_DP) {
        p = strchr(p, '{');
        if (!p) break;
        int d = 0; const char *q = p;
        while (*q) {
            if (*q=='{') d++; else if (*q=='}') { d--; if(!d){q++;break;} }
            q++;
        }
        size_t l = (size_t)(q - p);
        char *obj = (char*)malloc(l + 1);
        if (!obj) break;
        memcpy(obj, p, l); obj[l] = '\0';

        tuya_dp_t *dp = &dev->dp[dev->dp_count];
        json_str(obj, "code", dp->code, sizeof(dp->code));

        /* Wykryj typ wartości */
        const char *vp = strstr(obj, "\"value\":");
        if (vp) {
            vp += 8; while (*vp==' ') vp++;
            if (*vp == '"') {
                dp->type = DP_TYPE_STRING;
                vp++;
                const char *ve = strchr(vp, '"');
                size_t vl = ve ? (size_t)(ve-vp) : 0;
                if (vl >= sizeof(dp->value.s)) vl = sizeof(dp->value.s)-1;
                memcpy(dp->value.s, vp, vl); dp->value.s[vl] = '\0';
            } else if (strncmp(vp, "true", 4)==0 || strncmp(vp, "false", 5)==0) {
                dp->type = DP_TYPE_BOOL;
                dp->value.b = (*vp == 't');
            } else {
                dp->type = DP_TYPE_INT;
                dp->value.i = (int32_t)strtol(vp, NULL, 10);
            }
        }
        dp->last_update_s = (uint32_t)time(NULL);

        /* Callback dla silnika reguł */
        if (s_dp_cb && dp->code[0])
            s_dp_cb(dev->id, dp);

        if (dp->code[0]) dev->dp_count++;
        free(obj);
        p = q;
    }

    free(resp);
    return ESP_OK;
}

/* ── Wyślij komendę ──────────────────────────────────────────────────────── */
static esp_err_t send_command(const char *device_id, const char *body_json) {
    char path[96];
    snprintf(path, sizeof(path), "/v1.0/devices/%s/commands", device_id);
    char *resp = NULL;
    esp_err_t err = tuya_http_request(&s_ctx, "POST", path, body_json, &resp);
    if (err != ESP_OK || !resp) return err;
    bool ok = api_success(resp);
    if (!ok) ESP_LOGW(TAG, "Komenda nieudana: %s", resp);
    else ESP_LOGI(TAG, "Komenda wysłana do %s OK", device_id);
    free(resp);
    return ok ? ESP_OK : ESP_FAIL;
}

/* ── Stan → string ───────────────────────────────────────────────────────── */
static void push_state_event(tuya_state_t st) {
    const char *names[] = {"idle","connecting","ready","error","reconnect"};
    char ev[64];
    snprintf(ev, sizeof(ev), "{\"state\":\"%s\"}", names[(int)st < 5 ? st : 4]);
    http_server_push_event("tuya_state", ev);
}

/* ── Główny task ──────────────────────────────────────────────────────────── */
static void tuya_task(void *arg) {
    uint32_t poll_ticks = 0, heartbeat_ticks = 0;
    const uint32_t POLL_TICKS      = POLL_INTERVAL_MS  / 1000;
    const uint32_t HEARTBEAT_TICKS = HEARTBEAT_MS      / 1000;

    while (true) {
        switch (s_state) {

        case TUYA_STATE_IDLE:
        case TUYA_STATE_RECONNECT:
            s_state = TUYA_STATE_CONNECTING;
            push_state_event(TUYA_STATE_CONNECTING);
            /* fall-through */

        case TUYA_STATE_CONNECTING: {
            esp_err_t err = auth_get_token();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Autentykacja nieudana — czekam %ds", RECONNECT_DELAY_MS/1000);
                s_state = TUYA_STATE_ERROR;
                push_state_event(TUYA_STATE_ERROR);
                vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
                s_state = TUYA_STATE_RECONNECT;
                break;
            }
            /* Pobierz urządzenia po auth */
            fetch_devices();
            for (int i = 0; i < s_dev_cnt; i++)
                fetch_device_status(&s_devs[i]);

            s_state = TUYA_STATE_READY;
            push_state_event(TUYA_STATE_READY);
            poll_ticks = 0; heartbeat_ticks = 0;
            break;
        }

        case TUYA_STATE_READY:
            vTaskDelay(pdMS_TO_TICKS(1000));
            poll_ticks++;
            heartbeat_ticks++;

            /* Sprawdź czy token wymaga odświeżenia */
            if ((int64_t)time(NULL) >= s_ctx.token_expire_s - TOKEN_REFRESH_MARGIN_S) {
                ESP_LOGI(TAG, "Token wkrótce wygaśnie — odświeżam");
                if (auth_refresh_token() != ESP_OK) {
                    s_state = TUYA_STATE_RECONNECT;
                    break;
                }
            }

            /* Poll statusów DP */
            if (poll_ticks >= POLL_TICKS) {
                poll_ticks = 0;
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                for (int i = 0; i < s_dev_cnt; i++) {
                    if (fetch_device_status(&s_devs[i]) != ESP_OK) {
                        ESP_LOGW(TAG, "Błąd pobierania DP urządzenia %d", i);
                    }
                }
                xSemaphoreGive(s_mutex);
            }

            /* Heartbeat SSE */
            if (heartbeat_ticks >= HEARTBEAT_TICKS) {
                heartbeat_ticks = 0;
                char hb[64];
                snprintf(hb, sizeof(hb), "{\"state\":\"ready\",\"devices\":%d}", s_dev_cnt);
                http_server_push_event("tuya_heartbeat", hb);
            }
            break;

        case TUYA_STATE_ERROR:
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            s_state = TUYA_STATE_RECONNECT;
            break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   API PUBLICZNE
   ═══════════════════════════════════════════════════════════════════════════ */

esp_err_t tuya_client_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    /* Wczytaj zaszyfrowane dane TUYA */
    tuya_creds_t creds = {};
    esp_err_t err = crypto_load_tuya_creds(&creds, sizeof(creds));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Brak danych TUYA w NVS (jeszcze nie skonfigurowano)");
        return err;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    strncpy(s_ctx.client_id,     creds.client_id,     sizeof(s_ctx.client_id)-1);
    strncpy(s_ctx.client_secret, creds.client_secret, sizeof(s_ctx.client_secret)-1);
    strncpy(s_ctx.user_uid,      creds.user_uid,      sizeof(s_ctx.user_uid)-1);
    strncpy(s_ctx.region,        creds.region,        sizeof(s_ctx.region)-1);

    /* Wyczyść lokalną kopię creds */
    memset(&creds, 0, sizeof(creds));

    ESP_LOGI(TAG, "Klient TUYA zainicjalizowany (region=%s)", s_ctx.region);
    return ESP_OK;
}

esp_err_t tuya_client_start(void) {
    if (s_task) return ESP_ERR_INVALID_STATE;
    s_state = TUYA_STATE_IDLE;
    BaseType_t r = xTaskCreate(tuya_task, "tuya_client", 8192,
                                NULL, 5, &s_task);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t tuya_client_stop(void) {
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    s_state = TUYA_STATE_IDLE;
    return ESP_OK;
}

esp_err_t tuya_client_test(void) {
    ESP_LOGI(TAG, "Test połączenia TUYA…");
    /* Tymczasowy kontekst bez zmiany globalnego tokenu */
    tuya_http_ctx_t tmp = s_ctx;
    tmp.access_token[0] = '\0';
    char *resp = NULL;
    esp_err_t err = tuya_http_request(&tmp, "GET",
        "/v1.0/token?grant_type=1", NULL, &resp);
    bool ok = (err == ESP_OK && resp && api_success(resp));
    if (resp) free(resp);
    ESP_LOGI(TAG, "Test TUYA: %s", ok ? "OK" : "FAIL");
    return ok ? ESP_OK : ESP_FAIL;
}

tuya_state_t tuya_client_get_state(void)   { return s_state; }
int          tuya_client_get_device_count(void) { return s_dev_cnt; }

esp_err_t tuya_client_get_device(int idx, tuya_device_t *out) {
    if (idx < 0 || idx >= s_dev_cnt) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_devs[idx], sizeof(tuya_device_t));
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

const tuya_device_t *tuya_client_find_device(const char *id) {
    for (int i = 0; i < s_dev_cnt; i++)
        if (strcmp(s_devs[i].id, id) == 0) return &s_devs[i];
    return NULL;
}

esp_err_t tuya_client_send_bool(const char *dev_id,
                                  const char *dp_code, bool value) {
    char body[128];
    snprintf(body, sizeof(body),
        "{\"commands\":[{\"code\":\"%s\",\"value\":%s}]}",
        dp_code, value ? "true" : "false");
    return send_command(dev_id, body);
}

esp_err_t tuya_client_send_int(const char *dev_id,
                                 const char *dp_code, int32_t value) {
    char body[128];
    snprintf(body, sizeof(body),
        "{\"commands\":[{\"code\":\"%s\",\"value\":%ld}]}",
        dp_code, (long)value);
    return send_command(dev_id, body);
}

void tuya_client_set_dp_change_cb(tuya_dp_change_cb_t cb) { s_dp_cb = cb; }

char *tuya_client_devices_to_json(void) {
    /* Szacuj rozmiar: ~200B/urządzenie */
    size_t cap = 64 + s_dev_cnt * 220;
    char *buf = (char*)malloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;
    pos += snprintf(buf + pos, cap - pos, "{\"devices\":[");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_dev_cnt; i++) {
        const tuya_device_t *d = &s_devs[i];
        pos += snprintf(buf + pos, cap - pos,
            "%s{\"id\":\"%s\",\"name\":\"%s\","
            "\"product_id\":\"%s\",\"online\":%s,\"dp_count\":%d}",
            i > 0 ? "," : "",
            d->id, d->name, d->product_id,
            d->online ? "true" : "false", d->dp_count);
    }
    xSemaphoreGive(s_mutex);

    snprintf(buf + pos, cap - pos, "],\"total\":%d}", s_dev_cnt);
    return buf;
}
