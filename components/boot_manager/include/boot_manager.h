#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tryby pracy urządzenia.
 *
 * SETUP  – pierwsze uruchomienie lub uszkodzona konfiguracja:
 *          urządzenie wystawia punkt dostępu i formularz konfiguracyjny.
 * RUNTIME – konfiguracja poprawna, oba klienty mogą być inicjalizowane.
 */
typedef enum {
    BOOT_MODE_SETUP   = 0,
    BOOT_MODE_RUNTIME = 1
} boot_mode_t;

/**
 * @brief Inicjalizuje flash NVS, wyznacza tryb pracy i loguje informacje
 *        o partycji i przyczynie resetu.
 *
 * Musi być wywołana jako pierwsza, przed jakimkolwiek odczytem konfiguracji.
 *
 * @return Wyznaczony tryb pracy.
 */
boot_mode_t boot_manager_init(void);

/**
 * @brief Zwraca aktualny tryb pracy (wartość z ostatniego boot_manager_init).
 */
boot_mode_t boot_manager_get_mode(void);

/**
 * @brief Wymuś restart urządzenia po czasie [ms].
 *        Loguje powód przed wywołaniem esp_restart().
 */
void boot_manager_restart(const char *reason, uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
