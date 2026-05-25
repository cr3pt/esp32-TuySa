#pragma once
/**
 * watchdog.h — Monitorowanie łączności + sygnalizacja LED
 * ──────────────────────────────────────────────────────────
 *
 * Stany systemu i wzorce LED (GPIO konfigurowalne):
 *
 *  ┌──────────────────┬──────────────────────────────────────────────┐
 *  │ Stan             │ LED (domyślnie jedna dioda RGB lub 3 oddzielne)│
 *  ├──────────────────┼──────────────────────────────────────────────┤
 *  │ BOOT             │ Szybkie miganie 10 Hz (wszystkie stany)       │
 *  │ ALL_OK           │ Stałe świecenie zielone (lub 1 Hz zielone)    │
 *  │ WIFI_DOWN        │ Szybkie miganie czerwone 5 Hz                 │
 *  │ TUYA_RECONNECT   │ Wolne miganie żółte 1 Hz                      │
 *  │ SATEL_RECONNECT  │ Wolne miganie pomarańczowe 0.5 Hz             │
 *  │ BOTH_RECONNECT   │ Naprzemiennie żółte/pomarańczowe 2 Hz         │
 *  │ ERROR_CRITICAL   │ SOS w Morse (... --- ...)                     │
 *  └──────────────────┴──────────────────────────────────────────────┘
 *
 * GPIO LED (domyślne, zmień w menuconfig lub przez watchdog_config_t):
 *   GPIO_LED_GREEN  = 2
 *   GPIO_LED_YELLOW = 4
 *   GPIO_LED_RED    = 5
 *
 * Można użyć jednej diody RGB z kanałami PWM (LEDC) lub 3 oddzielnych GPIO.
 */
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Stany systemu ───────────────────────────────────────────────────────── */
typedef enum {
    SYS_STATE_BOOT            = 0,
    SYS_STATE_ALL_OK          = 1,
    SYS_STATE_WIFI_DOWN       = 2,
    SYS_STATE_TUYA_RECONNECT  = 3,
    SYS_STATE_SATEL_RECONNECT = 4,
    SYS_STATE_BOTH_RECONNECT  = 5,
    SYS_STATE_ERROR_CRITICAL  = 6,
} sys_state_t;

/* ── Konfiguracja watchdoga ──────────────────────────────────────────────── */
typedef struct {
    /* GPIO dla LED (użyj -1 aby wyłączyć dany kanał) */
    int gpio_led_green;    /* Domyślnie GPIO 2  */
    int gpio_led_yellow;   /* Domyślnie GPIO 4  */
    int gpio_led_red;      /* Domyślnie GPIO 5  */

    /* Użyj jednej diody (gpio_led_green) z logiką OR stanów */
    bool single_led_mode;

    /* Interwały sprawdzania (ms) */
    uint32_t check_interval_ms;    /* Domyślnie 3000 ms */

    /* Backoff reconnect WiFi */
    uint32_t wifi_reconnect_min_ms;   /* Domyślnie  5 000 ms */
    uint32_t wifi_reconnect_max_ms;   /* Domyślnie 60 000 ms */

    /* Backoff reconnect TUYA */
    uint32_t tuya_reconnect_min_ms;
    uint32_t tuya_reconnect_max_ms;

    /* Backoff reconnect SATEL */
    uint32_t satel_reconnect_min_ms;
    uint32_t satel_reconnect_max_ms;
} watchdog_config_t;

/* ── Health check odpowiedź (/api/status) ────────────────────────────────── */
typedef struct {
    sys_state_t state;
    const char *state_name;

    /* WiFi */
    bool     wifi_connected;
    char     wifi_ip[20];
    int8_t   wifi_rssi;

    /* TUYA */
    bool     tuya_connected;
    int      tuya_devices_count;
    uint32_t tuya_last_ok_s;
    uint32_t tuya_reconnect_count;

    /* SATEL */
    bool     satel_connected;
    char     satel_panel_type[32];
    uint32_t satel_last_ok_s;
    uint32_t satel_reconnect_count;
    int      satel_violated_zones;
    int      satel_armed_partitions;

    /* Silnik reguł */
    int      rules_total;
    int      rules_enabled;
    uint32_t rules_fired_total;

    /* System */
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t min_free_heap;
} sys_health_t;

/* ── API ──────────────────────────────────────────────────────────────────── */

/**
 * @brief Zainicjalizuj watchdog z domyślną konfiguracją.
 */
esp_err_t watchdog_init(void);

/**
 * @brief Zainicjalizuj watchdog z własną konfiguracją.
 */
esp_err_t watchdog_init_cfg(const watchdog_config_t *cfg);

/**
 * @brief Uruchom task watchdoga.
 */
esp_err_t watchdog_start(void);

/**
 * @brief Zatrzymaj task watchdoga.
 */
esp_err_t watchdog_stop(void);

/**
 * @brief Pobierz aktualny stan systemu.
 */
sys_state_t watchdog_get_state(void);

/**
 * @brief Pobierz pełny health check.
 */
esp_err_t watchdog_get_health(sys_health_t *out);

/**
 * @brief Serializuj health check do JSON (caller musi free()).
 */
char *watchdog_health_to_json(void);

/**
 * @brief Manualnie ustaw stan krytyczny (np. z obsługi błędu).
 */
void watchdog_set_critical(const char *reason);

/**
 * @brief Skasuj stan krytyczny.
 */
void watchdog_clear_critical(void);

/* Domyślne GPIO */
#define WATCHDOG_DEFAULT_GPIO_GREEN   2
#define WATCHDOG_DEFAULT_GPIO_YELLOW  4
#define WATCHDOG_DEFAULT_GPIO_RED     5

#ifdef __cplusplus
}
#endif
