#pragma once
#include "esp_err.h"
#include "esp_wifi.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stan połączenia WiFi.
 * Używany przez watchdog (etap 8) i WWW do raportowania statusu.
 */
typedef enum {
    WIFI_STATE_IDLE        = 0,
    WIFI_STATE_CONNECTING  = 1,
    WIFI_STATE_CONNECTED   = 2,
    WIFI_STATE_FAILED      = 3,
    WIFI_STATE_AP_ACTIVE   = 4,
} wifi_state_t;

/**
 * @brief Callback wywoływany po zmianie stanu połączenia.
 *        ip = 0 gdy rozłączono lub w trybie AP.
 */
typedef void (*wifi_state_cb_t)(wifi_state_t state, uint32_t ip);

/**
 * @brief Inicjalizacja stosu (esp_netif + event loop).
 *        Musi być wywołana jeden raz przed trybem AP lub STA.
 */
esp_err_t wifi_manager_init(void);

/* ── Tryb AP (Setup / Captive Portal) ───────────────────────────────────── */

/**
 * @brief Uruchom softAP o podanej nazwie (SSID) bez hasła.
 *        Używany przy pierwszej konfiguracji.
 *        SSID domyślnie: "ESP32-Bridge-Setup"
 */
esp_err_t wifi_manager_start_ap(const char *ssid);

/* ── Tryb STA (Runtime) ─────────────────────────────────────────────────── */

/**
 * @brief Połącz z siecią WiFi na podstawie zapisanej konfiguracji.
 *        Blokuje do momentu uzyskania IP lub upływu timeout_ms.
 *        Po połączeniu woła callback (jeśli ustawiony).
 */
esp_err_t wifi_manager_start_sta(uint32_t timeout_ms);

/**
 * @brief Ustaw callback zmiany stanu – może być NULL.
 */
void wifi_manager_set_state_cb(wifi_state_cb_t cb);

/**
 * @brief Zwraca aktualny stan połączenia.
 */
wifi_state_t wifi_manager_get_state(void);

/**
 * @brief Zwraca przydzielony adres IPv4 (0 gdy brak połączenia).
 */
uint32_t wifi_manager_get_ip(void);

/**
 * @brief Rozłącz i zatrzymaj WiFi.
 */
esp_err_t wifi_manager_stop(void);

#ifdef __cplusplus
}
#endif
