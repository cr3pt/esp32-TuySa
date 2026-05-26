#include "tuya_client.h"
#include "tuya_http.h"
#include "crypto_manager.h"
#include "http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "TUYA";

#define POLL_INTERVAL_MS     10000
#define TOKEN_MARGIN_S       300     /* odswiesz token 5 min przed wygasnieciem */
#define RECONNECT_DELAY_MS   8000

static tuya_state_t       s_state       = TUYA_STATE_IDLE;
static int                s_reconnects  = 0;
static TaskHandle_t       s_task        = NULL;
static SemaphoreHandle_t  s_mutex       = NULL;

static char     s_token[128]    = {};
static char     s_uid[48]       = {};
static uint32_t s_token_expire  = 0;   /* unix ts wygasniecia */

static tuya_device_t s_devices[TUYA_MAX_DEVICES] = {};
static int           s_dev_count = 0;

static tuya_dp_change_cb_t s_dp_cb      = NULL;
static void               *s_dp_user    = NULL;
static tuya_state_cb_t     s_state_cb   = NULL;
static void               *s_state_user = NULL;

/* ---- Pomocnicze JSON ---- */

/* Wyciagnij wartosc stringa z JSON: "key":"value" -> value */
static bool jstr(const char *j, const char *key, char *out, size_t olen) {
    char needle[64]; snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(j, needle); if (!p) return false;
    p += strlen(needle);
    const char *e = strchr(p, '"'); if (!e) return false;
    size_t l = (size_t)(e-p); if (l >= olen) l = olen-1;
    memcpy(out, p, l); out[l] = '\0'; return true;
}
/* Wyciagnij bool: "key":true/false */
static int jbool(const char *j, const char *key) {
    char needle[64]; snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(j, needle); if (!p) return -1;
    p += strlen(needle); while (*p == ' ') p++;
    if (strncmp(p, "true",  4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return -1;
}
/* Wyciagnij int: "key":1234 */
static bool jint(const char *j, const char *key, int32_t *out) {
    char needle[64]; snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(j, needle); if (!p) return false;
    p += strlen(needle); while (*p == ' ') p++;
    if (*p == '"') p++;
    *out = (int32_t)atoi(p); return true;
}

/* ---- Zmiana stanu ---- */
static void set_state(tuya_state_t st) {
    s_state = st;
    if (s_state_cb) s_state_cb(st, s_state_user);
    const char *names[] = {"idle","connecting","ready","polling","token_refresh","error","reconnect"};
    char ev[64]; snprintf(ev, sizeof(ev), "{\"state\":\"%s\"}", names[st < 7 ? st : 0]);
    http_server_push_event("tuya_state", ev);
    ESP_LOGI(TAG, "Stan: %s", names[st < 7 ? st : 0]);
}

/* ---- Parsowanie listy urzadzen ---- */
/*
 * Odpowiedz: {"result":{"devices":[{"id":"...","name":"...","category":"...","online":true,...}]}}
 * Iterujemy po obiektach JSON szukajac id/name/category/online
 */
static int parse_devices(const char *json) {
    if (!json) return 0;
    const char *p = strstr(json, "\"devices\":");
    if (!p) p = json;
    int count = 0;
    while (count < TUYA_MAX_DEVICES) {
        /* Szukaj poczatku obiektu urzadzenia */
        const char *obj = strchr(p, '{');
        if (!obj) break;
        /* Szukaj konca obiektu (prosta heurystyka - nastepne '}') */
        const char *obj_end = obj + 1;
        int depth = 1;
        while (*obj_end && depth > 0) {
            if (*obj_end == '{') depth++;
            else if (*obj_end == '}') depth--;
            obj_end++;
        }
        char buf[512];
        size_t blen = (size_t)(obj_end - obj);
        if (blen >= sizeof(buf)) blen = sizeof(buf)-1;
        memcpy(buf, obj, blen); buf[blen] = '\0';

        char id[TUYA_DEV_ID_LEN] = {};
        if (!jstr(buf, "id", id, sizeof(id)) || !id[0]) { p = obj_end; continue; }

        tuya_device_t *d = &s_devices[count];
        memset(d, 0, sizeof(*d));
        strncpy(d->id,   id,  sizeof(d->id)-1);
        jstr(buf, "name",     d->name,     sizeof(d->name)-1);
        jstr(buf, "category", d->category, sizeof(d->category)-1);
        int onl = jbool(buf, "online"); d->online = (onl == 1);
        d->last_seen = (uint32_t)time(NULL);
        count++;
        p = obj_end;
    }
    return count;
}

/* ---- Parsowanie statusu DP ---- */
/*
 * Odpowiedz: {"result":{"properties":[{"code":"switch_1","value":true,...}]}}
 * lub: {"result":[{"code":"switch_1","value":true}]}
 */
static int parse_dp_status(const char *json, tuya_device_t *dev) {
    if (!json) return 0;
    /* Szukaj tablicy properties lub result */
    const char *arr = strstr(json, "\"properties\":");
    if (!arr) arr = strstr(json, "\"result\":");
    if (!arr) return 0;
    arr = strchr(arr, '['); if (!arr) return 0;

    int count = 0;
    const char *p = arr + 1;
    while (count < TUYA_MAX_DP) {
        const char *obj = strchr(p, '{'); if (!obj) break;
        const char *obj_end = obj + 1;
        int depth = 1;
        while (*obj_end && depth > 0) {
            if (*obj_end == '{') depth++;
            else if (*obj_end == '}') depth--;
            obj_end++;
        }
        char buf[256];
        size_t blen = (size_t)(obj_end - obj);
        if (blen >= sizeof(buf)) blen = sizeof(buf)-1;
        memcpy(buf, obj, blen); buf[blen] = '\0';

        char code[32] = {};
        if (!jstr(buf, "code", code, sizeof(code)) || !code[0]) { p = obj_end; continue; }

        /* Znajdz lub utwórz DP w urzadzeniu */
        tuya_dp_t *dp = NULL;
        for (int i = 0; i < dev->dp_count; i++) {
            if (strcmp(dev->dp[i].code, code) == 0) { dp = &dev->dp[i]; break; }
        }
        if (!dp && dev->dp_count < TUYA_MAX_DP) {
            dp = &dev->dp[dev->dp_count++];
            strncpy(dp->code, code, sizeof(dp->code)-1);
        }
        if (!dp) { p = obj_end; continue; }

        /* Wykryj typ wartosci */
        const char *val_p = strstr(buf, "\"value\":");
        if (val_p) {
            val_p += 8; while (*val_p == ' ') val_p++;
            if (*val_p == '"') {
                /* string lub enum */
                char sv[32] = {};
                jstr(buf, "value", sv, sizeof(sv));
                dp->type = TUYA_DP_STRING;
                strncpy(dp->value.s, sv, sizeof(dp->value.s)-1);
            } else if (strncmp(val_p, "true", 4)==0 || strncmp(val_p, "false",5)==0) {
                dp->type    = TUYA_DP_BOOL;
                dp->value.b = (*val_p == 't');
            } else {
                int32_t iv = 0; jint(buf, "value", &iv);
                dp->type    = TUYA_DP_INT;
                dp->value.i = iv;
            }
        }
        count++;
        p = obj_end;
    }
    return count;
}

/* ---- Pobranie urzadzen z TUYA Cloud ---- */
static esp_err_t fetch_devices(void) {
    char path[128];
    snprintf(path, sizeof(path), "/v2.0/cloud/thing/device?page_size=20&page_no=1");
    tuya_resp_t r = {};
    esp_err_t e = tuya_http_get(path, s_token, &r);
    if (e != ESP_OK || !r.body) { tuya_resp_free(&r); return ESP_FAIL; }

    /* Sprawdz success */
    if (jbool(r.body, "success") != 1) {
        ESP_LOGW(TAG, "fetch_devices: %s", r.body);
        tuya_resp_free(&r); return ESP_FAIL;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_dev_count = parse_devices(r.body);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Pobrano %d urzadzen", s_dev_count);
    tuya_resp_free(&r);
    return ESP_OK;
}

/* ---- Pobranie statusu DP jednego urzadzenia ---- */
static esp_err_t fetch_device_status(tuya_device_t *dev) {
    char path[128];
    snprintf(path, sizeof(path),
             "/v2.0/cloud/thing/%s/shadow/properties", dev->id);
    tuya_resp_t r = {};
    esp_err_t e = tuya_http_get(path, s_token, &r);
    if (e != ESP_OK || !r.body) { tuya_resp_free(&r); return ESP_FAIL; }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    tuya_device_t prev = *dev;
    parse_dp_status(r.body, dev);

    /* Callbacki dla zmienionych DP */
    if (s_dp_cb) {
        for (int i = 0; i < dev->dp_count; i++) {
            tuya_dp_t *dp = &dev->dp[i];
            /* Szukaj poprzedniej wartosci */
            bool changed = true;
            for (int j = 0; j < prev.dp_count; j++) {
                if (strcmp(prev.dp[j].code, dp->code) == 0) {
                    if (dp->type == TUYA_DP_BOOL && dp->value.b == prev.dp[j].value.b) changed=false;
                    else if (dp->type == TUYA_DP_INT && dp->value.i == prev.dp[j].value.i) changed=false;
                    else if (dp->type == TUYA_DP_STRING && strcmp(dp->value.s, prev.dp[j].value.s)==0) changed=false;
                    break;
                }
            }
            if (changed) s_dp_cb(dev->id, dp, s_dp_user);
        }
    }
    xSemaphoreGive(s_mutex);
    tuya_resp_free(&r);
    return ESP_OK;
}

/* ---- Token refresh ---- */
static bool token_needs_refresh(void) {
    return (s_token_expire == 0 ||
            (uint32_t)time(NULL) + TOKEN_MARGIN_S >= s_token_expire);
}

static esp_err_t refresh_token(void) {
    set_state(TUYA_STATE_TOKEN_REFRESH);
    uint32_t expire = 0;
    esp_err_t e = tuya_http_get_token(s_token, sizeof(s_token), &expire);
    if (e == ESP_OK) {
        s_token_expire = (uint32_t)time(NULL) + expire;
        ESP_LOGI(TAG, "Token OK, wygasa za %lus", (unsigned long)expire);
    } else {
        ESP_LOGE(TAG, "Blad pobrania tokena");
        s_token[0] = '\0'; s_token_expire = 0;
    }
    return e;
}

/* ---- Glowna petla tasku ---- */
static void tuya_task(void *) {
    while (true) {
        set_state(TUYA_STATE_CONNECTING);

        if (refresh_token() != ESP_OK) {
            set_state(TUYA_STATE_ERROR);
            s_reconnects++;
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            set_state(TUYA_STATE_RECONNECT);
            continue;
        }

        if (fetch_devices() != ESP_OK) {
            set_state(TUYA_STATE_ERROR);
            s_reconnects++;
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            set_state(TUYA_STATE_RECONNECT);
            continue;
        }

        /* Inicjalny status wszystkich urzadzen */
        for (int i = 0; i < s_dev_count; i++)
            fetch_device_status(&s_devices[i]);

        set_state(TUYA_STATE_READY);

        /* Petla pollingu */
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

            /* Refresh tokena jesli potrzebny */
            if (token_needs_refresh()) {
                if (refresh_token() != ESP_OK) break;
            }

            set_state(TUYA_STATE_POLLING);
            bool any_fail = false;
            for (int i = 0; i < s_dev_count; i++) {
                if (fetch_device_status(&s_devices[i]) != ESP_OK)
                    any_fail = true;
                vTaskDelay(pdMS_TO_TICKS(200)); /* throttle */
            }

            if (any_fail) {
                ESP_LOGW(TAG, "Blad pollingu, retry za %ds", RECONNECT_DELAY_MS/1000);
                break;
            }
            set_state(TUYA_STATE_READY);
        }

        set_state(TUYA_STATE_ERROR);
        s_reconnects++;
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
        set_state(TUYA_STATE_RECONNECT);
    }
}

/* ---- API publiczne ---- */
esp_err_t tuya_client_init(void) {
    tuya_creds_t c = {};
    if (crypto_load_tuya_creds(&c, sizeof(c)) != ESP_OK) return ESP_ERR_NOT_FOUND;
    tuya_http_set_credentials(c.region[0] ? c.region : "eu",
                               c.client_id, c.client_secret);
    strncpy(s_uid, c.user_uid, sizeof(s_uid)-1);
    s_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Init: region=%s cid=%s", c.region, c.client_id);
    return ESP_OK;
}

esp_err_t tuya_client_start(void) {
    return xTaskCreate(tuya_task, "tuya_client", 10240, NULL, 5, &s_task)
           == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t tuya_client_stop(void) {
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    return ESP_OK;
}

esp_err_t tuya_client_test(void) {
    char tok[128] = {}; uint32_t exp = 0;
    return tuya_http_get_token(tok, sizeof(tok), &exp);
}

tuya_state_t tuya_client_get_state(void)           { return s_state; }
int          tuya_client_get_reconnect_count(void) { return s_reconnects; }
int          tuya_client_get_device_count(void)    { return s_dev_count; }

bool tuya_client_get_device(int idx, tuya_device_t *out) {
    if (idx < 0 || idx >= s_dev_count) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_devices[idx];
    xSemaphoreGive(s_mutex);
    return true;
}

bool tuya_client_find_device(const char *id, tuya_device_t *out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_dev_count; i++) {
        if (strcmp(s_devices[i].id, id) == 0) { *out = s_devices[i]; xSemaphoreGive(s_mutex); return true; }
    }
    xSemaphoreGive(s_mutex);
    return false;
}

bool tuya_client_get_dp(const char *dev_id, const char *dp_code, tuya_dp_t *out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_dev_count; i++) {
        if (strcmp(s_devices[i].id, dev_id) == 0) {
            for (int j = 0; j < s_devices[i].dp_count; j++) {
                if (strcmp(s_devices[i].dp[j].code, dp_code) == 0) {
                    *out = s_devices[i].dp[j]; xSemaphoreGive(s_mutex); return true;
                }
            }
        }
    }
    xSemaphoreGive(s_mutex);
    return false;
}

/* ---- Sterowanie urzadzeniem ---- */
static esp_err_t send_command(const char *dev_id, const char *json_body) {
    char path[128];
    snprintf(path, sizeof(path), "/v2.0/cloud/thing/%s/shadow/properties/issue", dev_id);
    tuya_resp_t r = {};
    esp_err_t e = tuya_http_post(path, s_token, json_body, &r);
    if (e == ESP_OK && r.body)
        ESP_LOGI(TAG, "CMD %s -> %s", dev_id, r.body);
    tuya_resp_free(&r);
    return e;
}

esp_err_t tuya_cmd_bool(const char *dev_id, const char *dp_code, bool val) {
    char body[128];
    snprintf(body, sizeof(body),
             "{\"properties\":{\"%s\":%s}}", dp_code, val?"true":"false");
    return send_command(dev_id, body);
}

esp_err_t tuya_cmd_int(const char *dev_id, const char *dp_code, int32_t val) {
    char body[128];
    snprintf(body, sizeof(body),
             "{\"properties\":{\"%s\":%ld}}", dp_code, (long)val);
    return send_command(dev_id, body);
}

esp_err_t tuya_cmd_string(const char *dev_id, const char *dp_code, const char *val) {
    char body[256];
    snprintf(body, sizeof(body),
             "{\"properties\":{\"%s\":\"%s\"}}", dp_code, val ? val : "");
    return send_command(dev_id, body);
}

/* ---- JSON ---- */
char *tuya_devices_to_json(void) {
    char *buf = (char*)malloc(4096);
    if (!buf) return NULL;
    size_t p = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    p += snprintf(buf+p, 4096-p, "{\"devices\":[");
    for (int i = 0; i < s_dev_count; i++) {
        tuya_device_t *d = &s_devices[i];
        if (i) p += snprintf(buf+p, 4096-p, ",");
        p += snprintf(buf+p, 4096-p,
                      "{\"id\":\"%s\",\"name\":\"%s\",\"category\":\"%s\","
                      "\"online\":%s,\"dp_count\":%d}",
                      d->id, d->name, d->category,
                      d->online?"true":"false", d->dp_count);
    }
    p += snprintf(buf+p, 4096-p, "],\"total\":%d}", s_dev_count);
    xSemaphoreGive(s_mutex);
    return buf;
}

char *tuya_status_to_json(void) {
    char *buf = (char*)malloc(256);
    if (!buf) return NULL;
    const char *names[] = {"idle","connecting","ready","polling","token_refresh","error","reconnect"};
    snprintf(buf, 256,
             "{\"connected\":%s,\"state\":\"%s\",\"devices\":%d,\"reconnects\":%d}",
             (s_state==TUYA_STATE_READY||s_state==TUYA_STATE_POLLING)?"true":"false",
             names[s_state<7?s_state:0], s_dev_count, s_reconnects);
    return buf;
}

void tuya_set_dp_change_cb(tuya_dp_change_cb_t cb, void *u) { s_dp_cb=cb; s_dp_user=u; }
void tuya_set_state_cb(tuya_state_cb_t cb, void *u)         { s_state_cb=cb; s_state_user=u; }
