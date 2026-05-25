#pragma once
#include "satel_protocol.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Stan klienta ─────────────────────────────────────────────────────────── */
typedef enum {
    SATEL_STATE_IDLE       = 0,
    SATEL_STATE_CONNECTING = 1,
    SATEL_STATE_READY      = 2,
    SATEL_STATE_ERROR      = 3,
    SATEL_STATE_RECONNECT  = 4,
} satel_state_t;

/* ── Zmienna centrali (dla silnika reguł) ────────────────────────────────── */
typedef enum {
    SATEL_VAR_ZONE_VIOLATION = 0,   /* Naruszenie wejścia */
    SATEL_VAR_ZONE_TAMPER,          /* Sabotaż wejścia */
    SATEL_VAR_ZONE_ALARM,           /* Alarm wejścia */
    SATEL_VAR_ZONE_BYPASS,          /* Blokada wejścia */
    SATEL_VAR_OUTPUT_STATE,         /* Stan wyjścia */
    SATEL_VAR_PART_ARMED,           /* Strefa uzbrojona */
    SATEL_VAR_PART_ALARM,           /* Alarm strefy */
    SATEL_VAR_TROUBLE,              /* Awaria systemu */
} satel_var_type_t;

typedef struct {
    satel_var_type_t type;
    uint8_t          number;   /* numer wejścia/wyjścia/strefy (1-based) */
    bool             value;
    uint32_t         timestamp_s;
} satel_variable_t;

/* ── Callback zmiany zmiennej (etap 7 — silnik reguł) ─────────────────────── */
typedef void (*satel_var_change_cb_t)(const satel_variable_t *var);

/* ── Info o centrali ──────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  type_code;          /* 0=INTEGRA 24, 2=INTEGRA 32, ... */
    char     type_name[32];
    char     firmware_version[16];
    uint16_t zones_count;
    uint16_t outputs_count;
    uint16_t partitions_count;
} satel_panel_info_t;

/* ── API ──────────────────────────────────────────────────────────────────── */
esp_err_t    satel_client_init(void);
esp_err_t    satel_client_start(void);
esp_err_t    satel_client_stop(void);
esp_err_t    satel_client_test(void);
satel_state_t satel_client_get_state(void);

/** Pobierz pełny snapshot stanu centrali */
esp_err_t    satel_client_get_state_snapshot(satel_state_t *st_out,
                                              satel_state_t *reserved);
const satel_state_t *satel_client_get_raw_state(void);

/** Sprawdź stan pojedynczego wejścia (numer 1..256) */
bool satel_client_zone_violated(uint8_t zone_num);
bool satel_client_zone_alarm   (uint8_t zone_num);
bool satel_client_zone_bypassed(uint8_t zone_num);
bool satel_client_output_active(uint8_t output_num);
bool satel_client_part_armed   (uint8_t part_num);

/** Komendy sterujące */
esp_err_t satel_client_arm  (uint8_t part_mask, uint8_t mode);
esp_err_t satel_client_disarm(uint8_t part_mask);
esp_err_t satel_client_output_on (uint8_t output_num);
esp_err_t satel_client_output_off(uint8_t output_num);

/**
 * @brief Wyzwól wejście IP ETHM-1 PLUS.
 *        ETHM-1 PLUS obsługuje wejścia IP jako wirtualne wejścia centrali.
 *        Komenda IP_INPUT_ON/OFF pozwala symulować naruszenie wejścia
 *        przez sieć TCP bez fizycznego okablowania.
 */
esp_err_t satel_client_ip_input_on (uint8_t input_num);
esp_err_t satel_client_ip_input_off(uint8_t input_num);

/** Info o centrali */
const satel_panel_info_t *satel_client_get_panel_info(void);

/** Zarejestruj callback zmiany zmiennych (etap 7) */
void satel_client_set_var_change_cb(satel_var_change_cb_t cb);

/** Serializuj stan do JSON dla panelu WWW */
char *satel_client_state_to_json(void);

#ifdef __cplusplus
}
#endif
