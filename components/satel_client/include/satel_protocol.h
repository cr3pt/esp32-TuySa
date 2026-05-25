#pragma once
/**
 * Protokół binarny SATEL INTEGRA / ETHM-1 PLUS
 * ──────────────────────────────────────────────
 * Format ramki:
 *   [0xFF][0xFE] <data...> [CRC_H][CRC_L] [0xFE][0x0D]
 *
 * Pola <data> mogą zawierać bajty 0xFE — wtedy są kodowane jako:
 *   0xFE 0xF0  →  0xFE
 *
 * CRC = suma wszystkich bajtów w <data> (przed kodowaniem)
 *
 * Ref: "INTEGRA – komunikacja w systemie" (Satel, protokół ETH TCP)
 */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Kody komend INTEGRA ─────────────────────────────────────────────────── */
#define SATEL_CMD_ZONES_VIOLATION       0x00  /* Naruszenia wejść */
#define SATEL_CMD_ZONES_TAMPER          0x01
#define SATEL_CMD_ZONES_ALARM           0x02
#define SATEL_CMD_ZONES_ALARM_MEMORY    0x03
#define SATEL_CMD_ZONES_BYPASSED        0x04
#define SATEL_CMD_ZONES_NO_VIOLATION    0x05  /* Wejścia bez naruszenia */
#define SATEL_CMD_ZONES_MASKED          0x10
#define SATEL_CMD_OUTPUTS_STATE         0x17  /* Stan wyjść */
#define SATEL_CMD_DOORS_OPENED          0x18
#define SATEL_CMD_PARTITIONS_ARMED      0x09  /* Strefy uzbrojone */
#define SATEL_CMD_PARTITIONS_ARMED_MODE1 0x0A
#define SATEL_CMD_PARTITIONS_ARMED_MODE2 0x0B
#define SATEL_CMD_PARTITIONS_ARMED_MODE3 0x0C
#define SATEL_CMD_PARTITIONS_ALARM      0x0D
#define SATEL_CMD_TROUBLES              0x1F  /* Awarie */
#define SATEL_CMD_TROUBLES_MEMORY       0x20
#define SATEL_CMD_INT_TROUBLES          0x26
#define SATEL_CMD_INTEGRA_TYPE          0x7E  /* Typ centrali */
#define SATEL_CMD_INTEGRA_VERSION       0x7C  /* Wersja firmware */
#define SATEL_CMD_NEW_DATA              0xFF  /* Czy są nowe dane */

/* Komendy sterujące (wymagają hasła użytkownika) */
#define SATEL_CMD_ARM_MODE0             0x80  /* Uzbrojenie tryb 0 */
#define SATEL_CMD_ARM_MODE1             0x81
#define SATEL_CMD_ARM_MODE2             0x82
#define SATEL_CMD_ARM_MODE3             0x83
#define SATEL_CMD_DISARM                0x84
#define SATEL_CMD_CLEAR_ALARM           0x85
#define SATEL_CMD_OUTPUT_ON             0x88  /* Włącz wyjście */
#define SATEL_CMD_OUTPUT_OFF            0x89  /* Wyłącz wyjście */
#define SATEL_CMD_OUTPUT_SWITCH         0x91

/* Wejścia IP ETHM-1 PLUS — symulacja wejść przez TCP */
#define SATEL_CMD_IP_INPUT_ON           0x94
#define SATEL_CMD_IP_INPUT_OFF          0x95

/* Maks. liczba obiektów w centrali INTEGRA 128/256 */
#define SATEL_MAX_ZONES     256
#define SATEL_MAX_OUTPUTS   256
#define SATEL_MAX_PARTS     32
#define SATEL_MAX_IP_INPUTS 128   /* Wejścia IP ETHM-1 PLUS */

/* ── Typy odpowiedzi stanu ───────────────────────────────────────────────── */
/* Każdy bit w tablicy bajtów odpowiada jednemu obiektowi (wejście/wyjście/strefa) */
typedef struct {
    uint8_t zones_violation [SATEL_MAX_ZONES   / 8];  /* 32 B */
    uint8_t zones_tamper    [SATEL_MAX_ZONES   / 8];
    uint8_t zones_alarm     [SATEL_MAX_ZONES   / 8];
    uint8_t zones_bypassed  [SATEL_MAX_ZONES   / 8];
    uint8_t outputs_state   [SATEL_MAX_OUTPUTS / 8];  /* 32 B */
    uint8_t parts_armed     [SATEL_MAX_PARTS   / 8];  /*  4 B */
    uint8_t parts_alarm     [SATEL_MAX_PARTS   / 8];
    uint8_t troubles        [32];
    uint32_t last_update_s;
} satel_state_t;

/* ── Pojedyncze wejście / wyjście / strefa (dla silnika reguł) ────────────── */
typedef struct {
    uint8_t  number;    /* 1..256 */
    char     name[32];  /* opcjonalnie — ETHM-1 PLUS może zwrócić nazwy */
    bool     active;
} satel_zone_t;

typedef struct {
    uint8_t  number;
    char     name[32];
    bool     active;
} satel_output_t;

/* ── Pomocnicze operacje na bitmapach ─────────────────────────────────────── */
static inline bool satel_bit_get(const uint8_t *bitmap, uint8_t n) {
    if (n == 0) return false;
    n--;  /* INTEGRA numeruje od 1 */
    return (bitmap[n / 8] & (1u << (n % 8))) != 0;
}

static inline void satel_bit_set(uint8_t *bitmap, uint8_t n, bool v) {
    if (n == 0) return; n--;
    if (v) bitmap[n/8] |=  (uint8_t)(1u << (n%8));
    else   bitmap[n/8] &= (uint8_t)~(1u << (n%8));
}

/* ── Kodowanie / dekodowanie ramki ──────────────────────────────────────── */
/**
 * @brief Zakoduj dane do ramki SATEL (z 0xFE escape i CRC).
 * @param data      Dane (bajt komendy + parametry)
 * @param data_len  Długość danych
 * @param frame     Bufor wyjściowy — musi mieć co najmniej data_len*2+6 B
 * @param frame_len [out] Rzeczywista długość ramki
 */
esp_err_t satel_encode_frame(const uint8_t *data, size_t data_len,
                               uint8_t *frame, size_t *frame_len);

/**
 * @brief Zdekoduj ramkę SATEL, sprawdź CRC, zwróć dane.
 * @param frame     Odebrany bufor surowy
 * @param frame_len Długość bufora
 * @param data      Bufor wyjściowy
 * @param data_len  [out] Długość zdekodowanych danych
 * @return ESP_ERR_INVALID_CRC jeśli CRC nie zgadza się
 *         ESP_ERR_INVALID_RESPONSE jeśli brak znaczników 0xFF 0xFE
 */
esp_err_t satel_decode_frame(const uint8_t *frame, size_t frame_len,
                               uint8_t *data, size_t *data_len);

/**
 * @brief Zakoduj hasło integracji (8 cyfr hex) do formatu INTEGRA.
 *        Hasło ASCII "1234" → bajty 0x12 0x34 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
 */
void satel_encode_password(const char *ascii_pass, uint8_t out[8]);

#ifdef __cplusplus
}
#endif
