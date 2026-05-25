#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback wywoływany po pomyślnym zapisaniu konfiguracji przez użytkownika.
 *        Po wywołaniu firmware powinno wykonać restart.
 */
typedef void (*portal_saved_cb_t)(void);

/**
 * @brief Uruchamia:
 *   1. Fałszywy serwer DNS (odpowiada na wszystkie zapytania adresem AP)
 *   2. Serwer HTTP z formularzem konfiguracyjnym
 *
 * Po zapisaniu formularza i pozytywnej odpowiedzi wywołuje portal_saved_cb.
 */
esp_err_t captive_portal_start(portal_saved_cb_t cb);

/**
 * @brief Zatrzymuje oba serwery (DNS + HTTP).
 */
esp_err_t captive_portal_stop(void);

#ifdef __cplusplus
}
#endif
