#include "rule_engine.h"
#include "config_manager.h"
#include "tuya_client.h"
#include "satel_client.h"
#include "automation_modes.h"
#include "mqtt_bridge.h"
#include "webhook_client.h"
#include "event_log.h"
#include "system_services.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "RULES";
#define RULE_ENGINE_TICK_MS 15000

static TaskHandle_t s_task = NULL;
static uint32_t s_sched_last_fired_minute[MAX_SCHEDULES]; /* epoch/60 ostatniego odpalenia, 0 = nigdy */

/* --- Stan eskalacji --- */
static bool     s_escal_active = false;
static bool     s_escal_acked  = false;
static uint32_t s_escal_deadline = 0;   /* epoch, kiedy odpalic kolejny poziom */
static uint8_t  s_escal_next_level = 0;
static char     s_escal_reason[64] = {};

/* --- Sceny --- */
static void run_action(const scene_action_t *a) {
    if (strcmp(a->code, "tuya_bool") == 0) {
        tuya_cmd_bool(a->target, a->param, strcmp(a->value, "true") == 0 || strcmp(a->value, "1") == 0);
    } else if (strcmp(a->code, "tuya_int") == 0) {
        tuya_cmd_int(a->target, a->param, (int32_t)atoi(a->value));
    } else if (strcmp(a->code, "satel_output_on") == 0) {
        satel_output_on((uint8_t)atoi(a->target));
    } else if (strcmp(a->code, "satel_output_off") == 0) {
        satel_output_off((uint8_t)atoi(a->target));
    } else if (strcmp(a->code, "satel_arm") == 0) {
        satel_arm((uint8_t)atoi(a->target), (uint8_t)atoi(a->param));
    } else if (strcmp(a->code, "satel_disarm") == 0) {
        satel_disarm((uint8_t)atoi(a->target));
    } else {
        ESP_LOGW(TAG, "Nieznany kod akcji sceny: %s", a->code);
        return;
    }
    event_log_add(EV_INFO, "scene", "Akcja %s target=%s param=%s value=%s", a->code, a->target, a->param, a->value);
}

esp_err_t rule_engine_run_scene(const char *scene_name) {
    if (!scene_name || !scene_name[0]) return ESP_ERR_INVALID_ARG;
    scenes_config_t sc = {};
    if (config_manager_load_scenes(&sc) != ESP_OK) return ESP_ERR_NOT_FOUND;
    for (int i = 0; i < sc.count && i < MAX_SCENES; i++) {
        scene_t *s = &sc.items[i];
        if (!s->enabled || strcmp(s->name, scene_name) != 0) continue;
        ESP_LOGI(TAG, "Uruchamiam scene '%s' (%d akcji)", s->name, s->action_count);
        event_log_add(EV_INFO, "scene", "Scena '%s' uruchomiona", s->name);
        for (int a = 0; a < s->action_count && a < MAX_SCENE_ACTIONS; a++) run_action(&s->actions[a]);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Scena '%s' nie znaleziona lub wylaczona", scene_name);
    return ESP_ERR_NOT_FOUND;
}

/* --- Harmonogramy --- */
static void check_schedules(void) {
    schedules_config_t sc = {};
    if (config_manager_load_schedules(&sc) != ESP_OK) return;
    uint32_t now = system_services_get_epoch();
    if (!now) return; /* czas jeszcze nie zsynchronizowany przez NTP */
    time_t t = (time_t)now;
    struct tm tmv = {};
    localtime_r(&t, &tmv);
    uint8_t day_bit = (uint8_t)((tmv.tm_wday + 6) % 7); /* tm_wday: 0=niedziela -> bit6 (Nd), 1=poniedzialek -> bit0 (Pn) */
    uint32_t cur_minute = now / 60;

    for (int i = 0; i < sc.count && i < MAX_SCHEDULES; i++) {
        schedule_t *s = &sc.items[i];
        if (!s->enabled) continue;
        if (!((s->days_mask >> day_bit) & 1)) continue;
        if (s->hour != tmv.tm_hour || s->minute != tmv.tm_min) continue;
        if (s_sched_last_fired_minute[i] == cur_minute) continue; /* juz odpalony w tej minucie */
        s_sched_last_fired_minute[i] = cur_minute;

        ESP_LOGI(TAG, "Harmonogram '%s' wyzwolony", s->name);
        event_log_add(EV_INFO, "schedule", "Harmonogram '%s' wyzwolony", s->name);
        if (s->scene_name[0]) rule_engine_run_scene(s->scene_name);
        if (s->set_mode) automation_modes_set((bridge_mode_t)s->target_mode);
    }
}

/* --- Eskalacja --- */
static void fire_escalation_level(const escalation_level_t *lv, int level_num) {
    char payload[192];
    snprintf(payload, sizeof(payload), "{\"level\":%d,\"reason\":\"%s\",\"note\":\"%s\"}", level_num, s_escal_reason, lv->note);
    if (lv->via_webhook) webhook_client_post_json("/escalation/level", payload);
    if (lv->via_mqtt) mqtt_bridge_publish_json("escalation", payload, false);
    event_log_add(EV_WARN, "escalation", "Poziom %d (%s)%s%s", level_num, s_escal_reason,
                  lv->note[0] ? " - " : "", lv->note[0] ? lv->note : "");
}

void rule_engine_trigger_escalation(const char *reason) {
    escalation_config_t ec = {};
    if (config_manager_load_escalation(&ec) != ESP_OK || !ec.enabled || ec.level_count == 0) return;
    if (s_escal_active && !s_escal_acked) return; /* juz trwa */
    s_escal_active = true;
    s_escal_acked = false;
    s_escal_next_level = 0;
    snprintf(s_escal_reason, sizeof(s_escal_reason), "%s", reason ? reason : "alarm");
    s_escal_deadline = system_services_get_epoch() + ec.ack_timeout_sec;
    ESP_LOGW(TAG, "Eskalacja wyzwolona: %s (potwierdzenie w %us)", s_escal_reason, ec.ack_timeout_sec);
    event_log_add(EV_WARN, "escalation", "Wyzwolona: %s - potwierdz w ciagu %us, inaczej ruszy poziom 1", s_escal_reason, ec.ack_timeout_sec);
}

void rule_engine_ack_escalation(void) {
    if (!s_escal_active) return;
    s_escal_acked = true;
    s_escal_active = false;
    ESP_LOGI(TAG, "Eskalacja potwierdzona przez operatora");
    event_log_add(EV_INFO, "escalation", "Potwierdzona przez operatora (%s)", s_escal_reason);
}

bool rule_engine_escalation_active(void) { return s_escal_active && !s_escal_acked; }

static void check_escalation(void) {
    if (!s_escal_active || s_escal_acked) return;
    escalation_config_t ec = {};
    if (config_manager_load_escalation(&ec) != ESP_OK || !ec.enabled) { s_escal_active = false; return; }
    uint32_t now = system_services_get_epoch();
    if (now < s_escal_deadline) return;
    if (s_escal_next_level >= ec.level_count) { s_escal_active = false; return; } /* brak kolejnych poziomow */

    const escalation_level_t *lv = &ec.levels[s_escal_next_level];
    fire_escalation_level(lv, s_escal_next_level + 1);
    s_escal_deadline = now + (lv->after_sec ? lv->after_sec : 1);
    s_escal_next_level++;
}

static void rule_engine_task(void *) {
    while (true) {
        check_schedules();
        check_escalation();
        vTaskDelay(pdMS_TO_TICKS(RULE_ENGINE_TICK_MS));
    }
}

esp_err_t rule_engine_init(void) {
    memset(s_sched_last_fired_minute, 0, sizeof(s_sched_last_fired_minute));
    s_escal_active = false; s_escal_acked = false; s_escal_next_level = 0;
    return ESP_OK;
}
esp_err_t rule_engine_start(void) {
    if (s_task) return ESP_OK;
    return xTaskCreate(rule_engine_task, "rule_engine", 4096, NULL, 4, &s_task) == pdPASS ? ESP_OK : ESP_FAIL;
}
esp_err_t rule_engine_stop(void) {
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    return ESP_OK;
}

char *rule_engine_status_json(void) {
    char *buf = (char*)malloc(224);
    if (!buf) return NULL;
    snprintf(buf, 224,
        "{\"escalation_active\":%s,\"escalation_level\":%d,\"escalation_reason\":\"%s\"}",
        (s_escal_active && !s_escal_acked) ? "true" : "false", s_escal_next_level, s_escal_reason);
    return buf;
}
