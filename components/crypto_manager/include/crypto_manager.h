#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Rozmiary buforów szyfrowania.
 *
 * Każdy zaszyfrowany blob ma postać:
 *   [IV 16B][HMAC-SHA256 32B][ciphertext N B][padding do 16B]
 *
 * Maksymalna długość plaintext = CRYPTO_PLAIN_MAX
 * Maksymalna długość bloba     = CRYPTO_BLOB_MAX
 */
#define CRYPTO_PLAIN_MAX   256
#define CRYPTO_BLOB_MAX    (16 + 32 + CRYPTO_PLAIN_MAX + 16)  /* IV+HMAC+ct+pad */

/**
 * @brief Inicjalizuje moduł: wczytuje klucz szyfrujący z NVS.
 *        Musi być wywołane po config_manager_init().
 *
 * @return ESP_OK              — klucz załadowany, moduł gotowy
 *         ESP_ERR_NOT_FOUND   — klucz nie ustawiony (pierwsze uruchomienie)
 *         ESP_ERR_INVALID_CRC — klucz uszkodzony (integralność naruszona)
 */
esp_err_t crypto_manager_init(void);

/**
 * @brief Zwraca true jeśli klucz jest załadowany i moduł gotowy do użycia.
 */
bool crypto_manager_is_ready(void);

/**
 * @brief Ustawia klucz szyfrujący z podanego hasła (PBKDF2-HMAC-SHA256).
 *        Operacja dozwolona TYLKO gdy klucz nie był jeszcze ustawiony.
 *        Po ustawieniu klucza kolejne próby zwracają ESP_ERR_INVALID_STATE.
 *
 * @param passphrase  Hasło wpisane przez użytkownika (z formularza setup)
 * @param len         Długość hasła (min 12 znaków)
 */
esp_err_t crypto_manager_set_key_from_passphrase(const char *passphrase,
                                                  size_t len);

/**
 * @brief Szyfruje blok danych.
 *        Format wyjścia: [IV 16B][HMAC 32B][ciphertext + padding]
 *        Generuje losowe IV przy każdym wywołaniu.
 *
 * @param plain      Dane wejściowe (plaintext)
 * @param plain_len  Długość danych (maks. CRYPTO_PLAIN_MAX)
 * @param out        Bufor wyjściowy (musi mieć CRYPTO_BLOB_MAX bajtów)
 * @param out_len    [out] Rzeczywista długość zaszyfrowanego bloba
 */
esp_err_t crypto_encrypt(const uint8_t *plain, size_t plain_len,
                          uint8_t *out, size_t *out_len);

/**
 * @brief Deszyfruje blob zaszyfrowany przez crypto_encrypt().
 *        Weryfikuje HMAC przed deszyfrowaniem — jeśli HMAC niepoprawny,
 *        zwraca ESP_ERR_INVALID_CRC i nie modyfikuje bufora wyjściowego.
 *
 * @param blob      Zaszyfrowany blob
 * @param blob_len  Długość bloba
 * @param plain     Bufor wyjściowy (musi mieć CRYPTO_PLAIN_MAX bajtów)
 * @param plain_len [out] Rzeczywista długość odszyfrowanych danych
 */
esp_err_t crypto_decrypt(const uint8_t *blob, size_t blob_len,
                          uint8_t *plain, size_t *plain_len);

/**
 * @brief Zaszyfruj strukturę tuya_creds_t i zapisz do NVS jako blob.
 */
esp_err_t crypto_save_tuya_creds(const void *creds, size_t creds_size);

/**
 * @brief Odszyfruj i wczytaj strukturę tuya_creds_t z NVS.
 */
esp_err_t crypto_load_tuya_creds(void *creds, size_t creds_size);

/**
 * @brief Zaszyfruj strukturę satel_creds_t i zapisz do NVS.
 */
esp_err_t crypto_save_satel_creds(const void *creds, size_t creds_size);

/**
 * @brief Odszyfruj i wczytaj strukturę satel_creds_t z NVS.
 */
esp_err_t crypto_load_satel_creds(void *creds, size_t creds_size);

/**
 * @brief Wymaż wszystkie zaszyfrowane sekrety z NVS (factory reset).
 */
esp_err_t crypto_erase_secrets(void);

#ifdef __cplusplus
}
#endif
