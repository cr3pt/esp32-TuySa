/**
 * watchdog.cpp — Monitorowanie łączności + sygnalizacja LED
 * ──────────────────────────────────────────────────────────
 *
 * Dwa współpracujące taski FreeRTOS:
 *
 *  watchdog_monitor_task  (prio=8, co check_interval_ms)
 *    ● odczyt stanów WiFi / TUYA / SATEL
 *    ● oblicz sys_state_t
 *    ● przy utracie połączenia → inicjuj reconnect z backoff
 *    ● push SSE event do panelu WWW
 *
 *  watchdog_led_task  (prio=9, 50 ms tick)
 *    ● odczytuje s_state i generuje wzorzec migania
 *    ● SOS w kodzie Morse'a dla ERROR_CRITICAL
 */

#include "watchdog.h"
#include "tuya_client.h"
#include "satel_client.h"
#include "wifi_manager.h"
#include "http_server.h"
#include "rule_engine.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "WATCHDOG";

/* ── Wzorce LED: tablica par {on_ms, off_ms}, zakończona {0,0} ───────────── */
typedef struct { uint16_t on_ms; uint16_t off_ms; } led_step_t;

/* ALL_OK: wolne 1 Hz zielone */
static const led_step_t PAT_OK[]          = {{900,100},{0,0}};
/* WIFI_DOWN: szybkie 5 Hz czerwone */
static const led_step_t PAT_WIFI_DOWN[]   = {{100,100},{100,100},{100,100},{0,0}};
/* TUYA_RECONNECT: miganie 1 Hz żółte */
static const led_step_t PAT_TUYA_RC[]     = {{500,500},{0,0}};
/* SATEL_RECONNECT: 0.5 Hz pomarańczowe (żółty + czerwony) */
static const led_step_t PAT_SATEL_RC[]    = {{1000,1000},{0,0}};
/* BOTH_RECONNECT: naprzemiennie 2 Hz */
static const led_step_t PAT_BOTH_RC[]     = {{250,250},{250,250},{0,0}};
/* SOS: ... --- ... (Morse) */
static const led_step_t PAT_SOS[] = {
    {150,150},{150,150},{150,150},       /* S ... */
    {450,150},{450,150},{450,150},       /* O --- */
    {150,150},{150,150},{150,500},       /* S ... */
    {0,0}
};
/* BOOT: szybkie 10 Hz */
static const led_step_t PAT_BOOT[]        = {{50,50},{0,0}};

/* ── Backoff state ────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t delay_ms;
    uint32_t min_ms;
    uint32_t max_ms;
    uint32_t count;
} backoff_t;

static void backoff_reset(backoff_t *b) {
    b->delay_ms = b->min_ms;
    b->count    = 0;
}

static uint32_t backoff_next(backoff_t *b) {
    uint32_t d = b->delay_ms;
    b->delay_ms *= 2;
    if (b->delay_ms > b->max_ms) b->delay_ms = b->max_ms;
    b->count++;
    return d;
}

/* ── Stan globalny ───────────────────────────────────────────────────────── */
static watchdog_config_t s_cfg = {};
static sys_state_t       s_state   = SYS_STATE_BOOT;
static bool              s_critical = false;
static char              s_critical_reason[64] = {};
static SemaphoreHandle_t s_mutex   = NULL;
static TaskHandle_t      s_mon_task = NULL;
static TaskHandle_t      s_led_task = NULL;
static uint32_t          s_start_s  = 0;

static backoff_t s_wifi_backoff  = {};
static backoff_t s_tuya_backoff  = {};
static backoff_t s_satel_backoff = {};

/* Liczniki reconnectów */
static uint32_t s_tuya_rc_count  = 0;
static uint32_t s_satel_rc_count = 0;
static uint32_t s_tuya_last_ok   = 0;
static uint32_t s_satel_last_ok  = 0;

/* ── Konfiguracja GPIO ────────────────────────────────────────────────────── */
static void led_gpio_init(int gpio) {
    if (gpio < 0) return;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << gpio);
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t)gpio, 0);
}

static void led_set(int gpio, bool on) {
    if (gpio >= 0) gpio_set_level((gpio_num_t)gpio, on ? 1 : 0);
}

static void led_all_off(void) {
    led_set(s_cfg.gpio_led_green,  false);
    led_set(s_cfg.gpio_led_yellow, false);
    led_set(s_cfg.gpio_led_red,    false);
}

/* ── Wykonaj jeden krok wzorca LED ──────────────────────────────────────── */
typedef struct {
    int gpio_a;  /* główny kanał */
    int gpio_b;  /* opcjonalny drugi kanał (BOTH_RECONNECT) */
    bool toggle; /* czy naprzemiennie */
} led_channel_t;

static void play_pattern(const led_step_t *pat, led_channel_t ch) {
    for (int i = 0; pat[i].on_ms || pat[i].off_ms; i++) {
        led_all_off();
        if (ch.toggle && (i & 1)) {
            if (ch.gpio_b >= 0) led_set(ch.gpio_b, true);
        } else {
            if (ch.gpio_a >= 0) led_set(ch.gpio_a, true);
        }
        vTaskDelay(pdMS_TO_TICKS(pat[i].on_ms));
        led_all_off();
        if (pat[i].off_ms)
            vTaskDelay(pdMS_TO_TICKS(pat[i].off_ms));
    }
}

/* ── Task LED ────────────────────────────────────────────────────────────── */
static void watchdog_led_task(void *arg) {
    while (true) {
        sys_state_t st;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        st = s_state;
        xSemaphoreGive(s_mutex);

        switch (st) {
        case SYS_STATE_BOOT:
            play_pattern(PAT_BOOT, {s_cfg.gpio_led_yellow, -1, false});
            break;

        case SYS_STATE_ALL_OK:
            play_pattern(PAT_OK, {s_cfg.gpio_led_green, -1, false});
            break;

        case SYS_STATE_WIFI_DOWN:
            play_pattern(PAT_WIFI_DOWN, {s_cfg.gpio_led_red, -1, false});
            break;

        case SYS_STATE_TUYA_RECONNECT:
            play_pattern(PAT_TUYA_RC, {s_cfg.gpio_led_yellow, -1, false});
            break;

        case SYS_STATE_SATEL_RECONNECT:
            /* Pomarańczowe = żółty + czerwony jednocześnie */
            led_set(s_cfg.gpio_led_yellow, true);
            led_set(s_cfg.gpio_led_red,    true);
            vTaskDelay(pdMS_TO_TICKS(1000));
            led_all_off();
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;

        case SYS_STATE_BOTH_RECONNECT:
            /* Naprzemiennie żółty / czerwony */
            play_pattern(PAT_BOTH_RC, {
                s_cfg.gpio_led_yellow,
                s_cfg.gpio_led_red,
                true
            });
            break;

        case SYS_STATE_ERROR_CRITICAL:
            play_pattern(PAT_SOS, {s_cfg.gpio_led_red, -1, false});
            vTaskDelay(pdMS_TO_TICKS(2000));  /* pauza między SOS */
            break;

        default:
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        }
    }
}

/* ── Oblicz nowy stan systemu ────────────────────────────────────────────── */
static sys_state_t compute_state(bool wifi_ok, bool tuya_ok, bool satel_ok) {
    if (s_critical)          return SYS_STATE_ERROR_CRITICAL;
    if (!wifi_ok)            return SYS_STATE_WIFI_DOWN;
    if (!tuya_ok && !satel_ok) return SYS_STATE_BOTH_RECONNECT;
    if (!tuya_ok)            return SYS_STATE_TUYA_RECONNECT;
    if (!satel_ok)           return SYS_STATE_SATEL_RECONNECT;
    return SYS_STATE_ALL_OK;
}

/* ── SSE push przy zmianie stanu ─────────────────────────────────────────── */
static const char *state_name(sys_state_t st) {
    switch(st) {
    case SYS_STATE_BOOT:            return "boot";
    case SYS_STATE_ALL_OK:          return "ok";
    case SYS_STATE_WIFI_DOWN:       return "wifi_down";
    case SYS_STATE_TUYA_RECONNECT:  return "tuya_reconnect";
    case SYS_STATE_SATEL_RECONNECT: return "satel_reconnect";
    case SYS_STATE_BOTH_RECONNECT:  return "both_reconnect";
    case SYS_STATE_ERROR_CRITICAL:  return "critical";
    default:                        return "unknown";
    }
}

/* ── Task monitora ────────────────────────────────────────────────────────── */
static void watchdog_monitor_task(void *arg) {
    sys_state_t last_state = SYS_STATE_BOOT;
    uint32_t    check_tick = 0;
    const uint32_t CHECK_T = s_cfg.check_interval_ms / 10;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        check_tick++;
        if (check_tick < CHECK_T) continue;
        check_tick = 0;

        /* ── Pobierz stany ──────────────────────────────────────────── */
        bool wifi_ok  = wifi_manager_is_connected();
        bool tuya_ok  = (tuya_client_get_state()  == TUYA_STATE_READY);
        bool satel_ok = (satel_client_get_state() == SATEL_STATE_READY);

        uint32_t now = (uint32_t)time(NULL);
        if (tuya_ok)  s_tuya_last_ok  = now;
        if (satel_ok) s_satel_last_ok = now;

        /* ── Backoff reconnect TUYA ────────────────────────────────── */
        if (!tuya_ok && wifi_ok) {
            uint32_t delay = backoff_next(&s_tuya_backoff);
            ESP_LOGW(TAG, "TUYA offline — próba reconnect za %lu ms (próba #%lu)",
                     (unsigned long)delay, (unsigned long)s_tuya_backoff.count);
            s_tuya_rc_count++;
            /* tuya_client sam obsługuje reconnect w swoim tasku;
               watchdog tylko sygnalizuje i loguje */
        } else if (tuya_ok) {
            backoff_reset(&s_tuya_backoff);
        }

        /* ── Backoff reconnect SATEL ────────────────────────────────── */
        if (!satel_ok && wifi_ok) {
            uint32_t delay = backoff_next(&s_satel_backoff);
            ESP_LOGW(TAG, "SATEL offline — próba reconnect za %lu ms (próba #%lu)",
                     (unsigned long)delay, (unsigned long)s_satel_backoff.count);
            s_satel_rc_count++;
        } else if (satel_ok) {
            backoff_reset(&s_satel_backoff);
        }

        /* ── WiFi reconnect ─────────────────────────────────────────── */
        if (!wifi_ok) {
            uint32_t delay = backoff_next(&s_wifi_backoff);
            ESP_LOGW(TAG, "WiFi down — próba reconnect za %lu ms",
                     (unsigned long)delay);
            vTaskDelay(pdMS_TO_TICKS(delay));
            wifi_manager_start_sta(10000);
        } else {
            backoff_reset(&s_wifi_backoff);
        }

        /* ── Oblicz stan systemu ────────────────────────────────────── */
        sys_state_t new_state = compute_state(wifi_ok, tuya_ok, satel_ok);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = new_state;
        xSemaphoreGive(s_mutex);

        /* ── SSE przy zmianie stanu ──────────────────────────────────── */
        if (new_state != last_state) {
            ESP_LOGI(TAG, "Stan systemu: %s → %s",
                     state_name(last_state), state_name(new_state));
            char ev[128];
            snprintf(ev, sizeof(ev),
                "{\"state\":\"%s\",\"wifi\":%s,\"tuya\":%s,\"satel\":%s}",
                state_name(new_state),
                wifi_ok  ? "true" : "false",
                tuya_ok  ? "true" : "false",
                satel_ok ? "true" : "false");
            http_server_push_event("sys_state", ev);
            last_state = new_state;
        }

        /* ── Heartbeat SSE co 30s ─────────────────────────────────────── */
        static uint32_t last_hb = 0;
        if (now - last_hb >= 30) {
            last_hb = now;
            uint32_t uptime = now - s_start_s;
            char hb[256];
            snprintf(hb, sizeof(hb),
                "{\"uptime\":%lu,\"free_heap\":%lu,"
                "\"wifi\":%s,\"tuya\":%s,\"satel\":%s,"
                "\"tuya_rc\":%lu,\"satel_rc\":%lu}",
                (unsigned long)uptime,
                (unsigned long)esp_get_free_heap_size(),
                wifi_ok  ? "true" : "false",
                tuya_ok  ? "true" : "false",
                satel_ok ? "true" : "false",
                (unsigned long)s_tuya_rc_count,
                (unsigned long)s_satel_rc_count);
            http_server_push_event("sys_heartbeat", hb);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   API PUBLICZNE
   ═══════════════════════════════════════════════════════════════════════ */

esp_err_t watchdog_init_cfg(const watchdog_config_t *cfg) {
    memcpy(&s_cfg, cfg, sizeof(watchdog_config_t));
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    /* Inicjalizuj GPIO LED */
    led_gpio_init(s_cfg.gpio_led_green);
    led_gpio_init(s_cfg.gpio_led_yellow);
    led_gpio_init(s_cfg.gpio_led_red);

    /* Backoff init */
    s_wifi_backoff  = { s_cfg.wifi_reconnect_min_ms,  s_cfg.wifi_reconnect_min_ms,
                        s_cfg.wifi_reconnect_max_ms,  0 };
    s_tuya_backoff  = { s_cfg.tuya_reconnect_min_ms,  s_cfg.tuya_reconnect_min_ms,
                        s_cfg.tuya_reconnect_max_ms,  0 };
    s_satel_backoff = { s_cfg.satel_reconnect_min_ms, s_cfg.satel_reconnect_min_ms,
                        s_cfg.satel_reconnect_max_ms, 0 };

    s_start_s = (uint32_t)time(NULL);
    ESP_LOGI(TAG, "Watchdog zainicjalizowany (GPIO G=%d Y=%d R=%d)",
             s_cfg.gpio_led_green, s_cfg.gpio_led_yellow, s_cfg.gpio_led_red);
    return ESP_OK;
}

esp_err_t watchdog_init(void) {
    watchdog_config_t cfg = {
        .gpio_led_green          = WATCHDOG_DEFAULT_GPIO_GREEN,
        .gpio_led_yellow         = WATCHDOG_DEFAULT_GPIO_YELLOW,
        .gpio_led_red            = WATCHDOG_DEFAULT_GPIO_RED,
        .single_led_mode         = false,
        .check_interval_ms       = 3000,
        .wifi_reconnect_min_ms   = 5000,
        .wifi_reconnect_max_ms   = 60000,
        .tuya_reconnect_min_ms   = 5000,
        .tuya_reconnect_max_ms   = 120000,
        .satel_reconnect_min_ms  = 5000,
        .satel_reconnect_max_ms  = 120000,
    };
    return watchdog_init_cfg(&cfg);
}

esp_err_t watchdog_start(void) {
    if (s_mon_task || s_led_task) return ESP_ERR_INVALID_STATE;

    BaseType_t r1 = xTaskCreate(watchdog_monitor_task, "wdog_monitor",
                                 4096, NULL, 8, &s_mon_task);
    BaseType_t r2 = xTaskCreate(watchdog_led_task,     "wdog_led",
                                 2048, NULL, 9, &s_led_task);
    if (r1 != pdPASS || r2 != pdPASS) {
        ESP_LOGE(TAG, "Nie można uruchomić tasków watchdoga");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Watchdog uruchomiony");
    return ESP_OK;
}

esp_err_t watchdog_stop(void) {
    if (s_mon_task) { vTaskDelete(s_mon_task); s_mon_task = NULL; }
    if (s_led_task) { vTaskDelete(s_led_task); s_led_task = NULL; }
    led_all_off();
    return ESP_OK;
}

sys_state_t watchdog_get_state(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sys_state_t st = s_state;
    xSemaphoreGive(s_mutex);
    return st;
}

esp_err_t watchdog_get_health(sys_health_t *out) {
    memset(out, 0, sizeof(*out));
    out->state      = watchdog_get_state();
    out->state_name = state_name(out->state);

    out->wifi_connected = wifi_manager_is_connected();
    wifi_manager_get_ip_str(out->wifi_ip, sizeof(out->wifi_ip));
    out->wifi_rssi = wifi_manager_get_rssi();

    out->tuya_connected      = (tuya_client_get_state() == TUYA_STATE_READY);
    out->tuya_devices_count  = tuya_client_get_device_count();
    out->tuya_last_ok_s      = s_tuya_last_ok;
    out->tuya_reconnect_count = s_tuya_rc_count;

    out->satel_connected      = (satel_client_get_state() == SATEL_STATE_READY);
    const satel_panel_info_t *pi = satel_client_get_panel_info();
    if (pi) strncpy(out->satel_panel_type, pi->type_name,
                    sizeof(out->satel_panel_type)-1);
    out->satel_last_ok_s       = s_satel_last_ok;
    out->satel_reconnect_count = s_satel_rc_count;

    /* Policz naruszone wejścia i uzbrojone strefy */
    const satel_state_t *ss = satel_client_get_raw_state();
    if (ss) {
        for (int n = 1; n <= 128; n++) {
            if (satel_bit_get(ss->zones_violation, (uint8_t)n))
                out->satel_violated_zones++;
            if (satel_bit_get(ss->parts_armed, (uint8_t)n))
                out->satel_armed_partitions++;
        }
    }

    rule_engine_stats_t rs = {};
    rule_engine_get_stats(&rs);
    out->rules_total        = rs.total_rules;
    out->rules_enabled      = rs.enabled_rules;
    out->rules_fired_total  = rs.total_fires;

    out->uptime_s    = (uint32_t)time(NULL) - s_start_s;
    out->free_heap   = esp_get_free_heap_size();
    out->min_free_heap = esp_get_minimum_free_heap_size();
    return ESP_OK;
}

char *watchdog_health_to_json(void) {
    sys_health_t h = {}; watchdog_get_health(&h);
    char *buf = (char*)malloc(768);
    if (!buf) return NULL;
    snprintf(buf, 768,
        "{"
        "\"state\":\"%s\","
        "\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d},"
        "\"tuya\":{\"connected\":%s,\"devices\":%d,"
                  "\"last_ok\":%lu,\"reconnects\":%lu},"
        "\"satel\":{\"connected\":%s,\"panel\":\"%s\","
                   "\"last_ok\":%lu,\"reconnects\":%lu,"
                   "\"violated_zones\":%d,\"armed_parts\":%d},"
        "\"rules\":{\"total\":%d,\"enabled\":%d,\"fired\":%lu},"
        "\"system\":{\"uptime\":%lu,\"free_heap\":%lu,\"min_heap\":%lu}"
        "}",
        h.state_name,
        h.wifi_connected  ? "true":"false", h.wifi_ip, h.wifi_rssi,
        h.tuya_connected  ? "true":"false", h.tuya_devices_count,
        (unsigned long)h.tuya_last_ok_s, (unsigned long)h.tuya_reconnect_count,
        h.satel_connected ? "true":"false", h.satel_panel_type,
        (unsigned long)h.satel_last_ok_s, (unsigned long)h.satel_reconnect_count,
        h.satel_violated_zones, h.satel_armed_partitions,
        h.rules_total, h.rules_enabled, (unsigned long)h.rules_fired_total,
        (unsigned long)h.uptime_s, (unsigned long)h.free_heap,
        (unsigned long)h.min_free_heap);
    return buf;
}

void watchdog_set_critical(const char *reason) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_critical = true;
    strncpy(s_critical_reason, reason ? reason : "unknown",
            sizeof(s_critical_reason)-1);
    s_state = SYS_STATE_ERROR_CRITICAL;
    xSemaphoreGive(s_mutex);
    ESP_LOGE(TAG, "BŁĄD KRYTYCZNY: %s", s_critical_reason);
    http_server_push_event("sys_critical",
        "{\"critical\":true,\"reason\":\"error\"}");
}

void watchdog_clear_critical(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_critical = false;
    s_critical_reason[0] = '\0';
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Stan krytyczny skasowany");
}
