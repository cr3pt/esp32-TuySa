#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Stałe ───────────────────────────────────────────────────────────────── */
#define CFG_SSID_LEN       64
#define CFG_PASS_LEN       64
#define CFG_HOST_LEN       64
#define CFG_NTP_LEN        64
#define CFG_DNS_LEN        16   /* xxx.xxx.xxx.xxx + '\0' */
#define CFG_IP_LEN         16
#define CFG_ENC_KEY_LEN    32   /* 256-bit klucz szyfrujący */
#define CFG_SECRET_LEN     128  /* max długość zaszyfrowanego sekretu */

/* ── Konfiguracja sieci ──────────────────────────────────────────────────── */
typedef struct {
    char     ssid[CFG_SSID_LEN];
    char     password[CFG_PASS_LEN];
    char     hostname[CFG_HOST_LEN];
    char     ntp_server[CFG_NTP_LEN];
    char     dns1[CFG_DNS_LEN];
    char     dns2[CFG_DNS_LEN];
    bool     dhcp;              /* true = DHCP, false = statyczne IP */
    char     static_ip[CFG_IP_LEN];
    char     static_gw[CFG_IP_LEN];
    char     static_nm[CFG_IP_LEN];
} net_config_t;

/* ── Dane dostępowe TUYA (przechowywane zaszyfrowane po etapie 4) ─────────── */
typedef struct {
    char region[8];             /* "eu", "us", "cn" */
    char client_id[64];
    char client_secret[64];
    char user_uid[64];
} tuya_creds_t;

/* ── Dane dostępowe SATEL ETHM-1 PLUS ───────────────────────────────────── */
typedef struct {
    char     host[CFG_IP_LEN];
    uint16_t port;
    char     password[32];
    char     panel_id[32];
} satel_creds_t;

/* ── API ──────────────────────────────────────────────────────────────────── */

/**
 * @brief Otwiera przestrzeń NVS i przygotowuje wewnętrzny cache.
 *        Musi być wywołana po nvs_flash_init().
 */
esp_err_t config_manager_init(void);

/**
 * @brief Zwraca true jeśli konfiguracja WiFi została już zapisana.
 */
bool config_manager_is_provisioned(void);

/* Sieć */
esp_err_t config_manager_save_net(const net_config_t *cfg);
esp_err_t config_manager_load_net(net_config_t *cfg);

/* TUYA */
esp_err_t config_manager_save_tuya(const tuya_creds_t *creds);
esp_err_t config_manager_load_tuya(tuya_creds_t *creds);

/* SATEL */
esp_err_t config_manager_save_satel(const satel_creds_t *creds);
esp_err_t config_manager_load_satel(satel_creds_t *creds);

/* Klucz szyfrujący urządzenia (używany od etapu 4) */
esp_err_t config_manager_save_enc_key(const uint8_t key[CFG_ENC_KEY_LEN]);
esp_err_t config_manager_load_enc_key(uint8_t key[CFG_ENC_KEY_LEN]);

/**
 * @brief Kasuje całą konfigurację (factory reset).
 */
esp_err_t config_manager_erase_all(void);

#ifdef __cplusplus
}
#endif

/* ── Struktury danych dostępowych (szyfrowane przez crypto_manager) ───────── */
typedef struct {
    char region[8];
    char client_id[48];
    char client_secret[48];
    char user_uid[48];
} tuya_creds_t;

typedef struct {
    char     host[64];
    uint16_t port;
    char     password[16];   /* ASCII, maks. 8 cyfr */
    char     panel_id[32];
} satel_creds_t;
