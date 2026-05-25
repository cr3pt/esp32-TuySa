#pragma once
#include "esp_err.h"
#include "tuya_client.h"
#include "satel_client.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
   TYPY REGUŁ
   ═══════════════════════════════════════════════════════════════════════ */

#define RULE_MAX_COUNT       32
#define RULE_ID_LEN          16
#define RULE_NAME_LEN        48
#define RULE_DEVICE_ID_LEN   TUYA_DEVICE_ID_LEN
#define RULE_DP_CODE_LEN     TUYA_DP_CODE_LEN
#define RULE_COOLDOWN_MIN_S  1    /* Minimalny cooldown między akcjami */

/* ── Źródła wyzwalacza ───────────────────────────────────────────────── */
typedef enum {
    TRIGGER_SATEL_ZONE_VIOLATION = 0,  /* Naruszenie wejścia SATEL */
    TRIGGER_SATEL_ZONE_ALARM,          /* Alarm wejścia SATEL */
    TRIGGER_SATEL_ZONE_TAMPER,         /* Sabotaż wejścia SATEL */
    TRIGGER_SATEL_ZONE_BYPASS,         /* Blokada wejścia SATEL */
    TRIGGER_SATEL_OUTPUT_STATE,        /* Zmiana stanu wyjścia SATEL */
    TRIGGER_SATEL_PART_ARMED,          /* Uzbrojenie strefy SATEL */
    TRIGGER_SATEL_PART_ALARM,          /* Alarm strefy SATEL */
    TRIGGER_TUYA_DP_BOOL,              /* Zmiana DP bool w TUYA */
    TRIGGER_TUYA_DP_INT,               /* Zmiana DP int w TUYA */
    TRIGGER_SCHEDULE,                  /* Wyzwalacz czasowy (cron-like) */
} trigger_type_t;

/* ── Warunek wyzwalacza ──────────────────────────────────────────────── */
typedef struct {
    trigger_type_t type;

    /* Dla SATEL: numer obiektu (1..256) */
    uint8_t  satel_number;

    /* Dla TUYA */
    char     tuya_device_id[RULE_DEVICE_ID_LEN];
    char     tuya_dp_code  [RULE_DP_CODE_LEN];

    /* Oczekiwana wartość wyzwalacza:
       - SATEL: true = aktywacja, false = dezaktywacja
       - TUYA bool: true/false
       - TUYA int: wartość dokładna
       - SCHEDULE: nieużywane (wyzwalacz zawsze przechodzi)    */
    bool     expect_bool;
    int32_t  expect_int;
    bool     expect_exact;   /* false = każda zmiana, true = tylko gdy = expect */

    /* Cooldown: minimalna przerwa między kolejnymi wyzwoleniami (sekundy) */
    uint32_t cooldown_s;
} rule_trigger_t;

/* ── Typy akcji ──────────────────────────────────────────────────────── */
typedef enum {
    ACTION_TUYA_SEND_BOOL = 0,     /* Wyślij DP bool do TUYA */
    ACTION_TUYA_SEND_INT,          /* Wyślij DP int do TUYA */
    ACTION_SATEL_OUTPUT_ON,        /* Włącz wyjście SATEL */
    ACTION_SATEL_OUTPUT_OFF,       /* Wyłącz wyjście SATEL */
    ACTION_SATEL_OUTPUT_SWITCH,    /* Przełącz wyjście SATEL */
    ACTION_SATEL_ARM,              /* Uzbrój strefy SATEL (maska) */
    ACTION_SATEL_DISARM,           /* Rozbrój strefy SATEL */
    ACTION_SATEL_IP_INPUT_ON,      /* Wejście IP ETHM-1 PLUS ON */
    ACTION_SATEL_IP_INPUT_OFF,     /* Wejście IP ETHM-1 PLUS OFF */
    ACTION_LOG_ONLY,               /* Tylko zapis do logu (tryb testowy) */
} action_type_t;

/* ── Definicja akcji ─────────────────────────────────────────────────── */
typedef struct {
    action_type_t type;

    /* TUYA */
    char     tuya_device_id[RULE_DEVICE_ID_LEN];
    char     tuya_dp_code  [RULE_DP_CODE_LEN];
    bool     value_bool;
    int32_t  value_int;

    /* SATEL */
    uint8_t  satel_number;      /* numer wyjścia/strefy/wejścia IP */
    uint8_t  satel_part_mask;   /* maska stref (arm/disarm) */
    uint8_t  satel_arm_mode;    /* tryb uzbrojenia 0..3 */

    /* Opóźnienie przed wykonaniem akcji (ms) */
    uint32_t delay_ms;
} rule_action_t;

/* ── Kompletna reguła ────────────────────────────────────────────────── */
typedef struct {
    char           id  [RULE_ID_LEN];    /* UUID skrócony, unikalny */
    char           name[RULE_NAME_LEN];
    bool           enabled;
    bool           dry_run;              /* true = LOG_ONLY, bez wykonania */
    uint8_t        priority;             /* 0 = najwyższy */
    rule_trigger_t trigger;
    rule_action_t  action;

    /* Runtime (nie zapisywane do NVS) */
    uint32_t       last_fired_s;
    uint32_t       fire_count;
} rule_t;

/* ═══════════════════════════════════════════════════════════════════════
   API SILNIKA REGUŁ
   ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Inicjalizuj silnik: wczytaj reguły z NVS, zarejestruj callbacki
 *        w tuya_client i satel_client.
 */
esp_err_t rule_engine_init(void);

/**
 * @brief Uruchom task silnika reguł.
 */
esp_err_t rule_engine_start(void);

/**
 * @brief Zatrzymaj task.
 */
esp_err_t rule_engine_stop(void);

/**
 * @brief Dodaj regułę. Zwraca ESP_ERR_NO_MEM jeśli limit RULE_MAX_COUNT.
 *        Automatycznie generuje UUID jako id jeśli rule->id jest puste.
 */
esp_err_t rule_engine_add(rule_t *rule);

/**
 * @brief Usuń regułę po ID.
 */
esp_err_t rule_engine_delete(const char *rule_id);

/**
 * @brief Pobierz regułę po indeksie.
 */
esp_err_t rule_engine_get(int idx, rule_t *out);

/**
 * @brief Zwraca liczbę reguł.
 */
int rule_engine_count(void);

/**
 * @brief Zapisz wszystkie reguły do NVS.
 */
esp_err_t rule_engine_save(void);

/**
 * @brief Serializuj reguły do JSON (dla /api/rules endpoint).
 *        Caller musi free() wynik.
 */
char *rule_engine_rules_to_json(void);

/**
 * @brief Deserializuj jedną regułę z JSON i dodaj do silnika.
 *        Używane przez POST /api/rules z panelu WWW.
 */
esp_err_t rule_engine_add_from_json(const char *json);

/**
 * @brief Ręcznie wyzwól regułę (do testów z panelu WWW).
 */
esp_err_t rule_engine_test_fire(const char *rule_id);

/**
 * @brief Statystyki silnika (dla panelu WWW).
 */
typedef struct {
    int      total_rules;
    int      enabled_rules;
    uint32_t total_fires;
    uint32_t last_fire_s;
    char     last_rule_name[RULE_NAME_LEN];
} rule_engine_stats_t;

esp_err_t rule_engine_get_stats(rule_engine_stats_t *out);

#ifdef __cplusplus
}
#endif
