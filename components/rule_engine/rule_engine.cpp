/**
 * rule_engine.cpp — Silnik reguł automatyki TUYA ↔ SATEL
 * ─────────────────────────────────────────────────────────
 *
 * Przepływ:
 *   Callback TUYA DP change  ──► match_and_queue()
 *   Callback SATEL var change ──► match_and_queue()
 *                                    │
 *                              QueueHandle_t (pending_actions)
 *                                    │
 *                           rule_executor_task()
 *                                    │
 *                  ┌─────────────────┴──────────────────┐
 *              execute_action()                   dry_run log
 *         ┌─────────┴──────────┐
 *    tuya_send_*()      satel_output/ip_input/arm()
 *
 * Serializacja reguł do NVS:
 *   namespace "bridge_rules"
 *   klucz "rule_N" (N = 0..RULE_MAX_COUNT-1) → blob rule_t
 *   klucz "rule_cnt" → uint8_t
 */

#include "rule_engine.h"
#include "tuya_client.h"
#include "satel_client.h"
#include "http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "RULES";

#define NVS_NS          "bridge_rules"
#define NVS_KEY_CNT     "rule_cnt"
#define NVS_KEY_FMT     "rule_%02d"
#define EXEC_QUEUE_SIZE 16

/* ── Zdarzenie do wykonania (z opóźnieniem) ──────────────────────────────── */
typedef struct {
    rule_t   rule;           /* kopia reguły w chwili wyzwolenia */
    uint32_t fire_at_ms;     /* xTaskGetTickCount() + delay */
} pending_exec_t;

/* ── Stan globalny ───────────────────────────────────────────────────────── */
static rule_t            s_rules[RULE_MAX_COUNT] = {};
static int               s_count    = 0;
static SemaphoreHandle_t s_mutex    = NULL;
static QueueHandle_t     s_execq    = NULL;
static TaskHandle_t      s_task     = NULL;
static nvs_handle_t      s_nvs      = 0;
static rule_engine_stats_t s_stats  = {};

/* ── UUID lite: 8 znaków hex ─────────────────────────────────────────────── */
static void gen_id(char out[RULE_ID_LEN]) {
    uint32_t r1 = esp_random(), r2 = esp_random();
    snprintf(out, RULE_ID_LEN, "%08lx%08lx",
             (unsigned long)r1, (unsigned long)r2);
    out[RULE_ID_LEN-1] = '\0';
}

/* ── Minimalistyczny JSON builder (bezpieczny sprintf) ───────────────────── */
static void jstr(char *buf, size_t *pos, size_t cap,
                  const char *key, const char *val) {
    *pos += snprintf(buf + *pos, cap - *pos,
        "\"%s\":\"%s\",", key, val ? val : "");
}
static void jint(char *buf, size_t *pos, size_t cap,
                  const char *key, int64_t val) {
    *pos += snprintf(buf + *pos, cap - *pos, "\"%s\":%lld,", key, (long long)val);
}
static void jbool(char *buf, size_t *pos, size_t cap,
                   const char *key, bool val) {
    *pos += snprintf(buf + *pos, cap - *pos,
        "\"%s\":%s,", key, val ? "true" : "false");
}

/* ── Minimalistyczny JSON parser (wyciąg wartości) ───────────────────────── */
static bool jp_str(const char *json, const char *key, char *out, size_t olen) {
    char needle[64]; snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle); if (!p) return false;
    p += strlen(needle); while(*p==' ')p++;
    if(*p!='"') return false; p++;
    const char *e = strchr(p, '"'); if(!e) return false;
    size_t l = (size_t)(e-p); if(l>=olen)l=olen-1;
    memcpy(out,p,l); out[l]='\0'; return true;
}
static bool jp_int(const char *json, const char *key, int64_t *out) {
    char needle[64]; snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle); if(!p) return false;
    p += strlen(needle); while(*p==' ')p++;
    *out = strtoll(p, NULL, 10); return true;
}
static bool jp_bool(const char *json, const char *key, bool *out) {
    char needle[64]; snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle); if(!p) return false;
    p += strlen(needle); while(*p==' ')p++;
    *out = (strncmp(p,"true",4)==0); return true;
}

/* ── NVS helpers ─────────────────────────────────────────────────────────── */
static esp_err_t nvs_open_rules(void) {
    if (s_nvs) return ESP_OK;
    return nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
}

/* ── Wykonaj akcję ───────────────────────────────────────────────────────── */
static void execute_action(const rule_t *r) {
    const rule_action_t *a = &r->action;

    if (r->dry_run || a->type == ACTION_LOG_ONLY) {
        ESP_LOGI(TAG, "[DRY-RUN] Reguła '%s' wyzwolona — akcja=%d",
                 r->name, a->type);
        return;
    }

    esp_err_t err = ESP_OK;
    switch (a->type) {

    case ACTION_TUYA_SEND_BOOL:
        ESP_LOGI(TAG, "TUYA send bool → dev=%s dp=%s val=%s",
                 a->tuya_device_id, a->tuya_dp_code,
                 a->value_bool ? "true" : "false");
        err = tuya_client_send_bool(a->tuya_device_id,
                                     a->tuya_dp_code, a->value_bool);
        break;

    case ACTION_TUYA_SEND_INT:
        ESP_LOGI(TAG, "TUYA send int → dev=%s dp=%s val=%ld",
                 a->tuya_device_id, a->tuya_dp_code, (long)a->value_int);
        err = tuya_client_send_int(a->tuya_device_id,
                                    a->tuya_dp_code, a->value_int);
        break;

    case ACTION_SATEL_OUTPUT_ON:
        ESP_LOGI(TAG, "SATEL output ON → #%u", a->satel_number);
        err = satel_client_output_on(a->satel_number);
        break;

    case ACTION_SATEL_OUTPUT_OFF:
        ESP_LOGI(TAG, "SATEL output OFF → #%u", a->satel_number);
        err = satel_client_output_off(a->satel_number);
        break;

    case ACTION_SATEL_OUTPUT_SWITCH:
        if (satel_client_output_active(a->satel_number))
            err = satel_client_output_off(a->satel_number);
        else
            err = satel_client_output_on(a->satel_number);
        ESP_LOGI(TAG, "SATEL output SWITCH → #%u", a->satel_number);
        break;

    case ACTION_SATEL_ARM:
        ESP_LOGI(TAG, "SATEL ARM → maska=0x%02X tryb=%u",
                 a->satel_part_mask, a->satel_arm_mode);
        err = satel_client_arm(a->satel_part_mask, a->satel_arm_mode);
        break;

    case ACTION_SATEL_DISARM:
        ESP_LOGI(TAG, "SATEL DISARM → maska=0x%02X", a->satel_part_mask);
        err = satel_client_disarm(a->satel_part_mask);
        break;

    case ACTION_SATEL_IP_INPUT_ON:
        ESP_LOGI(TAG, "SATEL IP INPUT ON → #%u", a->satel_number);
        err = satel_client_ip_input_on(a->satel_number);
        break;

    case ACTION_SATEL_IP_INPUT_OFF:
        ESP_LOGI(TAG, "SATEL IP INPUT OFF → #%u", a->satel_number);
        err = satel_client_ip_input_off(a->satel_number);
        break;

    default:
        ESP_LOGW(TAG, "Nieznany typ akcji: %d", a->type);
        break;
    }

    if (err != ESP_OK)
        ESP_LOGW(TAG, "Akcja reguły '%s' zakończona błędem 0x%x", r->name, err);

    /* SSE event */
    char ev[128];
    snprintf(ev, sizeof(ev),
        "{\"rule\":\"%s\",\"action\":%d,\"ok\":%s}",
        r->name, a->type, err == ESP_OK ? "true" : "false");
    http_server_push_event("rule_fired", ev);
}

/* ── Sprawdź czy reguła pasuje do zdarzenia SATEL ────────────────────────── */
static bool match_satel(const rule_t *r, const satel_variable_t *v) {
    const rule_trigger_t *t = &r->trigger;

    /* Sprawdź cooldown */
    uint32_t now = (uint32_t)time(NULL);
    if (t->cooldown_s > 0 &&
        now - r->last_fired_s < t->cooldown_s) return false;

    /* Mapuj typ zmiennej SATEL na trigger */
    switch (t->type) {
    case TRIGGER_SATEL_ZONE_VIOLATION:
        if (v->type != SATEL_VAR_ZONE_VIOLATION) return false; break;
    case TRIGGER_SATEL_ZONE_ALARM:
        if (v->type != SATEL_VAR_ZONE_ALARM)     return false; break;
    case TRIGGER_SATEL_ZONE_TAMPER:
        if (v->type != SATEL_VAR_ZONE_TAMPER)    return false; break;
    case TRIGGER_SATEL_ZONE_BYPASS:
        if (v->type != SATEL_VAR_ZONE_BYPASS)    return false; break;
    case TRIGGER_SATEL_OUTPUT_STATE:
        if (v->type != SATEL_VAR_OUTPUT_STATE)   return false; break;
    case TRIGGER_SATEL_PART_ARMED:
        if (v->type != SATEL_VAR_PART_ARMED)     return false; break;
    case TRIGGER_SATEL_PART_ALARM:
        if (v->type != SATEL_VAR_PART_ALARM)     return false; break;
    default: return false;
    }

    /* Numer obiektu: 0 = dowolny, N = konkretny */
    if (t->satel_number != 0 && t->satel_number != v->number) return false;

    /* Oczekiwana wartość */
    if (t->expect_exact && v->value != t->expect_bool) return false;

    return true;
}

/* ── Sprawdź czy reguła pasuje do zdarzenia TUYA DP ────────────────────── */
static bool match_tuya(const rule_t *r, const char *dev_id,
                         const tuya_dp_t *dp) {
    const rule_trigger_t *t = &r->trigger;

    if (t->type != TRIGGER_TUYA_DP_BOOL &&
        t->type != TRIGGER_TUYA_DP_INT)  return false;

    uint32_t now = (uint32_t)time(NULL);
    if (t->cooldown_s > 0 &&
        now - r->last_fired_s < t->cooldown_s) return false;

    /* ID urządzenia: puste = dowolne */
    if (t->tuya_device_id[0] &&
        strcmp(t->tuya_device_id, dev_id) != 0) return false;

    /* Kod DP: puste = dowolny */
    if (t->tuya_dp_code[0] &&
        strcmp(t->tuya_dp_code, dp->code) != 0) return false;

    if (!t->expect_exact) return true;

    if (t->type == TRIGGER_TUYA_DP_BOOL)
        return (dp->type == DP_TYPE_BOOL && dp->value.b == t->expect_bool);
    if (t->type == TRIGGER_TUYA_DP_INT)
        return (dp->type == DP_TYPE_INT  && dp->value.i == t->expect_int);
    return true;
}

/* ── Kolejkowanie wykonania z priorytetem ─────────────────────────────────── */
static void match_and_queue(rule_t *r) {
    pending_exec_t pe;
    memcpy(&pe.rule, r, sizeof(rule_t));
    pe.fire_at_ms = xTaskGetTickCount() * portTICK_PERIOD_MS
                    + r->action.delay_ms;

    if (xQueueSend(s_execq, &pe, 0) != pdTRUE)
        ESP_LOGW(TAG, "Kolejka wykonania pełna — reguła '%s' pominięta", r->name);
    else {
        r->last_fired_s = (uint32_t)time(NULL);
        r->fire_count++;
        /* Aktualizuj statystyki */
        s_stats.total_fires++;
        s_stats.last_fire_s = r->last_fired_s;
        strncpy(s_stats.last_rule_name, r->name, RULE_NAME_LEN-1);
    }
}

/* ── Callback SATEL ──────────────────────────────────────────────────────── */
static void on_satel_var_change(const satel_variable_t *v) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        rule_t *r = &s_rules[i];
        if (!r->enabled) continue;
        if (match_satel(r, v)) {
            ESP_LOGD(TAG, "Reguła '%s' pasuje do zdarzenia SATEL %d #%u=%d",
                     r->name, v->type, v->number, v->value);
            match_and_queue(r);
        }
    }
    xSemaphoreGive(s_mutex);
}

/* ── Callback TUYA ───────────────────────────────────────────────────────── */
static void on_tuya_dp_change(const char *dev_id, const tuya_dp_t *dp) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        rule_t *r = &s_rules[i];
        if (!r->enabled) continue;
        if (match_tuya(r, dev_id, dp)) {
            ESP_LOGD(TAG, "Reguła '%s' pasuje do zdarzenia TUYA dp=%s",
                     r->name, dp->code);
            match_and_queue(r);
        }
    }
    xSemaphoreGive(s_mutex);
}

/* ── Task wykonawczy ─────────────────────────────────────────────────────── */
static void rule_executor_task(void *arg) {
    pending_exec_t pe;
    while (true) {
        if (xQueueReceive(s_execq, &pe, pdMS_TO_TICKS(100)) == pdTRUE) {
            /* Respektuj opóźnienie akcji */
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (pe.fire_at_ms > now_ms) {
                vTaskDelay(pdMS_TO_TICKS(pe.fire_at_ms - now_ms));
            }
            execute_action(&pe.rule);
        }
    }
}

/* ── Wczytaj reguły z NVS ────────────────────────────────────────────────── */
static esp_err_t load_from_nvs(void) {
    esp_err_t err = nvs_open_rules();
    if (err != ESP_OK) return err;

    uint8_t cnt = 0;
    nvs_get_u8(s_nvs, NVS_KEY_CNT, &cnt);
    if (cnt > RULE_MAX_COUNT) cnt = RULE_MAX_COUNT;

    for (int i = 0; i < cnt; i++) {
        char key[16]; snprintf(key, sizeof(key), NVS_KEY_FMT, i);
        size_t sz = sizeof(rule_t);
        rule_t r = {};
        if (nvs_get_blob(s_nvs, key, &r, &sz) == ESP_OK && sz == sizeof(rule_t)) {
            r.last_fired_s = 0;   /* reset runtime fields */
            r.fire_count   = 0;
            s_rules[s_count++] = r;
            ESP_LOGI(TAG, "Wczytano regułę [%d]: '%s' (enabled=%d)",
                     i, r.name, r.enabled);
        }
    }
    ESP_LOGI(TAG, "Wczytano %d reguł z NVS", s_count);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
   API PUBLICZNE
   ═══════════════════════════════════════════════════════════════════════ */

esp_err_t rule_engine_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    s_execq = xQueueCreate(EXEC_QUEUE_SIZE, sizeof(pending_exec_t));
    if (!s_mutex || !s_execq) return ESP_ERR_NO_MEM;

    /* Załaduj reguły z NVS */
    load_from_nvs();

    /* Zarejestruj callbacki */
    satel_client_set_var_change_cb(on_satel_var_change);
    tuya_client_set_dp_change_cb  (on_tuya_dp_change);

    /* Statystyki początkowe */
    s_stats.total_rules   = s_count;
    s_stats.enabled_rules = 0;
    for (int i = 0; i < s_count; i++)
        if (s_rules[i].enabled) s_stats.enabled_rules++;

    ESP_LOGI(TAG, "Silnik reguł zainicjalizowany (%d reguł, %d aktywnych)",
             s_stats.total_rules, s_stats.enabled_rules);
    return ESP_OK;
}

esp_err_t rule_engine_start(void) {
    if (s_task) return ESP_ERR_INVALID_STATE;
    BaseType_t r = xTaskCreate(rule_executor_task, "rule_exec",
                                4096, NULL, 4, &s_task);
    if (r != pdPASS) return ESP_FAIL;
    ESP_LOGI(TAG, "Task silnika reguł uruchomiony");
    return ESP_OK;
}

esp_err_t rule_engine_stop(void) {
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    return ESP_OK;
}

esp_err_t rule_engine_add(rule_t *rule) {
    if (s_count >= RULE_MAX_COUNT) return ESP_ERR_NO_MEM;
    if (!rule->id[0]) gen_id(rule->id);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_rules[s_count++] = *rule;
    s_stats.total_rules = s_count;
    if (rule->enabled) s_stats.enabled_rules++;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Dodano regułę '%s' (id=%s)", rule->name, rule->id);
    return rule_engine_save();
}

esp_err_t rule_engine_delete(const char *rule_id) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_rules[i].id, rule_id) == 0) {
            /* Przesuń pozostałe */
            if (s_rules[i].enabled) s_stats.enabled_rules--;
            memmove(&s_rules[i], &s_rules[i+1],
                    (s_count - i - 1) * sizeof(rule_t));
            s_count--;
            s_stats.total_rules = s_count;
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "Usunięto regułę id=%s", rule_id);
            return rule_engine_save();
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t rule_engine_get(int idx, rule_t *out) {
    if (idx < 0 || idx >= s_count) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_rules[idx], sizeof(rule_t));
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

int rule_engine_count(void) { return s_count; }

esp_err_t rule_engine_save(void) {
    esp_err_t err = nvs_open_rules();
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_set_u8(s_nvs, NVS_KEY_CNT, (uint8_t)s_count);
    for (int i = 0; i < s_count; i++) {
        char key[16]; snprintf(key, sizeof(key), NVS_KEY_FMT, i);
        nvs_set_blob(s_nvs, key, &s_rules[i], sizeof(rule_t));
    }
    xSemaphoreGive(s_mutex);
    err = nvs_commit(s_nvs);
    ESP_LOGD(TAG, "Reguły zapisane do NVS (%d szt.)", s_count);
    return err;
}

/* ── JSON serialization ──────────────────────────────────────────────────── */
char *rule_engine_rules_to_json(void) {
    size_t cap = 128 + s_count * 512;
    char *buf  = (char*)malloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;
    pos += snprintf(buf+pos, cap-pos, "{\"rules\":[");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        const rule_t *r = &s_rules[i];
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf+pos, cap-pos, "{");
        jstr (buf,&pos,cap, "id",      r->id);
        jstr (buf,&pos,cap, "name",    r->name);
        jbool(buf,&pos,cap, "enabled", r->enabled);
        jbool(buf,&pos,cap, "dry_run", r->dry_run);
        jint (buf,&pos,cap, "priority",(int64_t)r->priority);
        jint (buf,&pos,cap, "fire_count",(int64_t)r->fire_count);
        jint (buf,&pos,cap, "last_fired",(int64_t)r->last_fired_s);
        /* trigger */
        pos += snprintf(buf+pos,cap-pos,"\"trigger\":{");
        jint (buf,&pos,cap, "type",     (int64_t)r->trigger.type);
        jint (buf,&pos,cap, "satel_num",(int64_t)r->trigger.satel_number);
        jstr (buf,&pos,cap, "tuya_dev", r->trigger.tuya_device_id);
        jstr (buf,&pos,cap, "tuya_dp",  r->trigger.tuya_dp_code);
        jbool(buf,&pos,cap, "expect_bool", r->trigger.expect_bool);
        jint (buf,&pos,cap, "expect_int", (int64_t)r->trigger.expect_int);
        jbool(buf,&pos,cap, "exact",    r->trigger.expect_exact);
        jint (buf,&pos,cap, "cooldown", (int64_t)r->trigger.cooldown_s);
        /* usuń trailing comma */
        if (buf[pos-1]==',') pos--;
        pos += snprintf(buf+pos,cap-pos,"},");
        /* action */
        pos += snprintf(buf+pos,cap-pos,"\"action\":{");
        jint (buf,&pos,cap, "type",     (int64_t)r->action.type);
        jstr (buf,&pos,cap, "tuya_dev", r->action.tuya_device_id);
        jstr (buf,&pos,cap, "tuya_dp",  r->action.tuya_dp_code);
        jbool(buf,&pos,cap, "val_bool", r->action.value_bool);
        jint (buf,&pos,cap, "val_int",  (int64_t)r->action.value_int);
        jint (buf,&pos,cap, "satel_num",(int64_t)r->action.satel_number);
        jint (buf,&pos,cap, "part_mask",(int64_t)r->action.satel_part_mask);
        jint (buf,&pos,cap, "arm_mode", (int64_t)r->action.satel_arm_mode);
        jint (buf,&pos,cap, "delay_ms", (int64_t)r->action.delay_ms);
        if (buf[pos-1]==',') pos--;
        pos += snprintf(buf+pos,cap-pos,"}");
        buf[pos++] = '}';
    }
    xSemaphoreGive(s_mutex);

    snprintf(buf+pos, cap-pos, "],\"total\":%d}", s_count);
    return buf;
}

/* ── JSON deserialization ────────────────────────────────────────────────── */
esp_err_t rule_engine_add_from_json(const char *json) {
    rule_t r = {}; r.enabled = true;
    int64_t v = 0;

    jp_str (json, "name",    r.name,    sizeof(r.name));
    jp_str (json, "id",      r.id,      sizeof(r.id));
    jp_bool(json, "enabled", &r.enabled);
    jp_bool(json, "dry_run", &r.dry_run);
    if (jp_int(json, "priority", &v)) r.priority = (uint8_t)v;

    /* Trigger */
    const char *trg = strstr(json, "\"trigger\":{");
    if (trg) {
        if (jp_int(trg,  "type",     &v)) r.trigger.type = (trigger_type_t)v;
        if (jp_int(trg,  "satel_num",&v)) r.trigger.satel_number = (uint8_t)v;
        jp_str (trg, "tuya_dev", r.trigger.tuya_device_id,
                sizeof(r.trigger.tuya_device_id));
        jp_str (trg, "tuya_dp",  r.trigger.tuya_dp_code,
                sizeof(r.trigger.tuya_dp_code));
        jp_bool(trg, "expect_bool", &r.trigger.expect_bool);
        if (jp_int(trg, "expect_int", &v)) r.trigger.expect_int = (int32_t)v;
        jp_bool(trg, "exact", &r.trigger.expect_exact);
        if (jp_int(trg, "cooldown",   &v)) r.trigger.cooldown_s = (uint32_t)v;
    }

    /* Action */
    const char *act = strstr(json, "\"action\":{");
    if (act) {
        if (jp_int(act, "type",     &v)) r.action.type = (action_type_t)v;
        jp_str (act, "tuya_dev", r.action.tuya_device_id,
                sizeof(r.action.tuya_device_id));
        jp_str (act, "tuya_dp",  r.action.tuya_dp_code,
                sizeof(r.action.tuya_dp_code));
        jp_bool(act, "val_bool", &r.action.value_bool);
        if (jp_int(act, "val_int",  &v)) r.action.value_int  = (int32_t)v;
        if (jp_int(act, "satel_num",&v)) r.action.satel_number = (uint8_t)v;
        if (jp_int(act, "part_mask",&v)) r.action.satel_part_mask = (uint8_t)v;
        if (jp_int(act, "arm_mode", &v)) r.action.satel_arm_mode  = (uint8_t)v;
        if (jp_int(act, "delay_ms", &v)) r.action.delay_ms = (uint32_t)v;
    }

    if (!r.name[0]) return ESP_ERR_INVALID_ARG;
    return rule_engine_add(&r);
}

/* ── Ręczne wyzwolenie (test z panelu) ───────────────────────────────────── */
esp_err_t rule_engine_test_fire(const char *rule_id) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_rules[i].id, rule_id) == 0) {
            rule_t copy = s_rules[i];
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "Ręczne wyzwolenie reguły '%s'", copy.name);
            execute_action(&copy);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t rule_engine_get_stats(rule_engine_stats_t *out) {
    memcpy(out, &s_stats, sizeof(rule_engine_stats_t));
    return ESP_OK;
}
