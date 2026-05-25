#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Stałe ───────────────────────────────────────────────────────────────── */
#define TUYA_MAX_DEVICES     32
#define TUYA_MAX_DP          16    /* Data Points na urządzenie w cache */
#define TUYA_DEVICE_ID_LEN   32
#define TUYA_DEVICE_NAME_LEN 64
#define TUYA_PRODUCT_LEN     32
#define TUYA_DP_CODE_LEN     32

/* ── Typy Data Points ────────────────────────────────────────────────────── */
typedef enum {
    DP_TYPE_BOOL    = 0,
    DP_TYPE_INT     = 1,
    DP_TYPE_STRING  = 2,
    DP_TYPE_ENUM    = 3,
    DP_TYPE_UNKNOWN = 99,
} dp_type_t;

typedef struct {
    char      code[TUYA_DP_CODE_LEN];
    dp_type_t type;
    union {
        bool     b;
        int32_t  i;
        char     s[64];
    } value;
    uint32_t  last_update_s;   /* czas unixa ostatniej aktualizacji */
} tuya_dp_t;

/* ── Urządzenie TUYA ─────────────────────────────────────────────────────── */
typedef struct {
    char      id[TUYA_DEVICE_ID_LEN];
    char      name[TUYA_DEVICE_NAME_LEN];
    char      product_id[TUYA_PRODUCT_LEN];
    bool      online;
    tuya_dp_t dp[TUYA_MAX_DP];
    uint8_t   dp_count;
} tuya_device_t;

/* ── Stan klienta ────────────────────────────────────────────────────────── */
typedef enum {
    TUYA_STATE_IDLE        = 0,
    TUYA_STATE_CONNECTING  = 1,
    TUYA_STATE_READY       = 2,   /* token aktywny, urządzenia pobrane */
    TUYA_STATE_ERROR       = 3,
    TUYA_STATE_RECONNECT   = 4,
} tuya_state_t;

/* ── Callback zmiany stanu DP (wywoływany przez silnik reguł, etap 7) ────── */
typedef void (*tuya_dp_change_cb_t)(const char *device_id,
                                    const tuya_dp_t *dp);

/* ── API ──────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicjalizuj klienta. Wczytuje zaszyfrowane dane z NVS przez
 *        crypto_manager. Nie nawiązuje połączenia.
 */
esp_err_t tuya_client_init(void);

/**
 * @brief Uruchom task FreeRTOS klienta TUYA.
 *        Task: auth → pobierz urządzenia → poll statusów → heartbeat.
 */
esp_err_t tuya_client_start(void);

/**
 * @brief Zatrzymaj task i wyczyść cache.
 */
esp_err_t tuya_client_stop(void);

/**
 * @brief Test połączenia (wywoływany przez /api/test z http_server).
 *        Synchroniczny — blokuje maks. 5s.
 */
esp_err_t tuya_client_test(void);

/**
 * @brief Zwraca aktualny stan klienta.
 */
tuya_state_t tuya_client_get_state(void);

/**
 * @brief Zwraca liczbę urządzeń w cache.
 */
int tuya_client_get_device_count(void);

/**
 * @brief Kopiuje urządzenie z cache pod podanym indeksem.
 *        Zwraca ESP_ERR_INVALID_ARG jeśli idx poza zakresem.
 */
esp_err_t tuya_client_get_device(int idx, tuya_device_t *out);

/**
 * @brief Znajdź urządzenie po ID. Zwraca NULL jeśli nie ma w cache.
 *        UWAGA: zwraca wskaźnik na wewnętrzny cache — używaj pod mutex.
 */
const tuya_device_t *tuya_client_find_device(const char *device_id);

/**
 * @brief Wyślij komendę do urządzenia.
 *        Np. włącz/wyłącz: dp_code="switch_1", value_bool=true
 */
esp_err_t tuya_client_send_bool(const char *device_id,
                                 const char *dp_code, bool value);
esp_err_t tuya_client_send_int (const char *device_id,
                                 const char *dp_code, int32_t value);

/**
 * @brief Zarejestruj callback zmiany DP (etap 7 — silnik reguł).
 */
void tuya_client_set_dp_change_cb(tuya_dp_change_cb_t cb);

/**
 * @brief Serializuj cache urządzeń do JSON (dla /api endpoint).
 *        Caller musi free() bufor.
 */
char *tuya_client_devices_to_json(void);

#ifdef __cplusplus
}
#endif
