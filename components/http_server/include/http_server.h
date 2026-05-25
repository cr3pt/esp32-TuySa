#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Typy odpowiedzi API ──────────────────────────────────────────────────── */
typedef enum {
    API_STATUS_OK      = 0,
    API_STATUS_ERROR   = 1,
    API_STATUS_PENDING = 2,
} api_status_t;

/**
 * @brief Callback wywoływany gdy REST API otrzyma żądanie testu połączenia.
 *        Implementacja (etap 5/6) wykona próbne połączenie do TUYA / SATEL.
 *        Zwróć ESP_OK przy sukcesie.
 */
typedef esp_err_t (*http_test_cb_t)(const char *target);  /* "tuya" | "satel" */

/**
 * @brief Callback wywoływany gdy API otrzyma żądanie factory-reset.
 */
typedef void (*http_reset_cb_t)(void);

/**
 * @brief Uruchom runtime serwer HTTP na porcie 80.
 *        Serwuje pliki statyczne z SPIFFS i obsługuje /api/*.
 *        Wymaga wcześniejszego spiffs_www_mount().
 *
 * @param username  Nazwa użytkownika do Basic Auth (może być NULL = brak auth)
 * @param password  Hasło do Basic Auth
 */
esp_err_t http_server_start(const char *username, const char *password);

/**
 * @brief Zatrzymaj serwer HTTP.
 */
esp_err_t http_server_stop(void);

/**
 * @brief Zarejestruj callback testu połączeń zewnętrznych.
 */
void http_server_set_test_cb(http_test_cb_t cb);

/**
 * @brief Zarejestruj callback factory-reset.
 */
void http_server_set_reset_cb(http_reset_cb_t cb);

/**
 * @brief Wyślij zdarzenie SSE do wszystkich otwartych klientów /api/events.
 *        Używane przez watchdog i klientów integracji do push-update UI.
 */
void http_server_push_event(const char *event_name, const char *json_data);

#ifdef __cplusplus
}
#endif

/* Etap 4: callback wywoływany gdy użytkownik ustawia klucz szyfrujący przez WWW */
typedef esp_err_t (*http_set_key_cb_t)(const char *passphrase);
void http_server_set_key_cb(http_set_key_cb_t cb);
