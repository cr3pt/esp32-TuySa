/**
 * Crypto Manager — ESP32 TUYA+SATEL Bridge
 * ─────────────────────────────────────────
 * Schemat ochrony sekretów:
 *
 *  1. PBKDF2-HMAC-SHA256(passphrase, device_salt, 10000 iter) → 32B master key
 *  2. master key + losowe IV (16B) → AES-256-CBC(plaintext + PKCS#7 padding)
 *  3. HMAC-SHA256(IV || ciphertext, master key) → 32B MAC
 *  4. Blob w NVS: [IV 16B][MAC 32B][ciphertext]
 *
 *  Dwa klucze pochodne z master key:
 *    enc_key  = HKDF(master, "enc")  → AES-256
 *    mac_key  = HKDF(master, "mac")  → HMAC
 *  Dzięki separacji klucz szyfrujący ≠ klucz uwierzytelniający.
 *
 *  Salt urządzenia = SHA256(ESP chip ID || "bridge-salt-v1") — unikalne per urządzenie.
 */

#include "crypto_manager.h"
#include "config_manager.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_efuse.h"
#include "nvs.h"
#include "nvs_flash.h"

/* mbedTLS */
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"   /* mbedtls_platform_zeroize */

#include <string.h>
#include <stdlib.h>

static const char *TAG = "CRYPTO";

/* ── Stałe ───────────────────────────────────────────────────────────────── */
#define AES_KEY_BITS     256
#define AES_KEY_BYTES    (AES_KEY_BITS / 8)   /* 32 */
#define AES_IV_BYTES     16
#define HMAC_BYTES       32
#define PBKDF2_ITER      10000
#define NVS_NS_CRYPTO    "bridge_sec"
#define NVS_KEY_SALT     "dev_salt"
#define NVS_KEY_TUYA     "sec_tuya"
#define NVS_KEY_SATEL    "sec_satel"
#define NVS_KEY_KEY_VER  "key_ver"   /* u8: 0=nie ustawiony, 1=ustawiony */

/* ── Stan wewnętrzny ─────────────────────────────────────────────────────── */
static uint8_t  s_enc_key[AES_KEY_BYTES] = {};
static uint8_t  s_mac_key[AES_KEY_BYTES] = {};
static bool     s_ready = false;
static nvs_handle_t s_nvs = 0;

/* ── Pomocnicze: NVS ─────────────────────────────────────────────────────── */
static esp_err_t nvs_open_crypto(void) {
    if (s_nvs) return ESP_OK;
    return nvs_open(NVS_NS_CRYPTO, NVS_READWRITE, &s_nvs);
}

static esp_err_t nvs_save_blob(const char *key, const void *data, size_t len) {
    esp_err_t e = nvs_set_blob(s_nvs, key, data, len);
    if (e == ESP_OK) e = nvs_commit(s_nvs);
    return e;
}

static esp_err_t nvs_load_blob(const char *key, void *data,
                                size_t *len) {
    return nvs_get_blob(s_nvs, key, data, len);
}

/* ── Generacja soli urządzenia ───────────────────────────────────────────── */
static void derive_device_salt(uint8_t salt[32]) {
    /* Użyj unikalnego ID chipu jako bazy soli */
    uint8_t chip_id[8] = {};
    esp_efuse_mac_get_default(chip_id);  /* 6-bajtowy MAC z eFuse */

    /* SHA256(chip_id || "bridge-salt-v1") */
    const char *suffix = "bridge-salt-v1";
    uint8_t buf[6 + 14];
    memcpy(buf, chip_id, 6);
    memcpy(buf + 6, suffix, 14);
    mbedtls_sha256(buf, sizeof(buf), salt, 0);
    ESP_LOGD(TAG, "Sól urządzenia wyliczona z eFuse MAC");
}

/* ── PBKDF2 → master key ──────────────────────────────────────────────────── */
static esp_err_t derive_master_key(const char *passphrase, size_t pass_len,
                                    const uint8_t salt[32],
                                    uint8_t master[AES_KEY_BYTES]) {
    const mbedtls_md_info_t *md =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        (const uint8_t *)passphrase, pass_len,
        salt, 32,
        PBKDF2_ITER,
        AES_KEY_BYTES, master);
    if (ret != 0) {
        ESP_LOGE(TAG, "PBKDF2 błąd: -0x%04x", (unsigned)(-ret));
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── HKDF — separacja kluczy enc / mac ──────────────────────────────────── */
static esp_err_t derive_subkeys(const uint8_t master[AES_KEY_BYTES]) {
    const uint8_t *info_enc = (const uint8_t *)"enc";
    const uint8_t *info_mac = (const uint8_t *)"mac";

    int r1 = mbedtls_hkdf(
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
        NULL, 0,           /* salt = brak (master jest już rozciągniętym kluczem) */
        master, AES_KEY_BYTES,
        info_enc, 3,
        s_enc_key, AES_KEY_BYTES);

    int r2 = mbedtls_hkdf(
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
        NULL, 0,
        master, AES_KEY_BYTES,
        info_mac, 3,
        s_mac_key, AES_KEY_BYTES);

    if (r1 != 0 || r2 != 0) {
        ESP_LOGE(TAG, "HKDF błąd r1=%d r2=%d", r1, r2);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── PKCS#7 padding ──────────────────────────────────────────────────────── */
static size_t pkcs7_pad(uint8_t *buf, size_t data_len, size_t block) {
    uint8_t pad = (uint8_t)(block - (data_len % block));
    for (size_t i = data_len; i < data_len + pad; i++) buf[i] = pad;
    return data_len + pad;
}

static esp_err_t pkcs7_unpad(uint8_t *buf, size_t *len, size_t block) {
    if (*len == 0 || *len % block != 0) return ESP_ERR_INVALID_SIZE;
    uint8_t pad = buf[*len - 1];
    if (pad == 0 || pad > block) return ESP_ERR_INVALID_CRC;
    for (size_t i = *len - pad; i < *len; i++)
        if (buf[i] != pad) return ESP_ERR_INVALID_CRC;
    *len -= pad;
    return ESP_OK;
}

/* ── HMAC-SHA256 ─────────────────────────────────────────────────────────── */
static esp_err_t compute_hmac(const uint8_t *data, size_t len,
                               uint8_t mac[HMAC_BYTES]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int r = mbedtls_md_setup(&ctx,
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    if (r == 0) r = mbedtls_md_hmac_starts(&ctx, s_mac_key, AES_KEY_BYTES);
    if (r == 0) r = mbedtls_md_hmac_update(&ctx, data, len);
    if (r == 0) r = mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);
    return (r == 0) ? ESP_OK : ESP_FAIL;
}

/* Porównanie w czasie stałym (zapobiega timing attack) */
static bool hmac_equal(const uint8_t a[HMAC_BYTES],
                        const uint8_t b[HMAC_BYTES]) {
    uint8_t diff = 0;
    for (int i = 0; i < HMAC_BYTES; i++) diff |= a[i] ^ b[i];
    return (diff == 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
   API PUBLICZNE
   ═══════════════════════════════════════════════════════════════════════════ */

esp_err_t crypto_manager_init(void) {
    esp_err_t err = nvs_open_crypto();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open błąd 0x%x", err); return err; }

    /* Sprawdź flagę wersji klucza */
    uint8_t key_ver = 0;
    nvs_get_u8(s_nvs, NVS_KEY_KEY_VER, &key_ver);
    if (key_ver == 0) {
        ESP_LOGW(TAG, "Klucz szyfrujący nie ustawiony — wymagana konfiguracja");
        return ESP_ERR_NOT_FOUND;
    }

    /* Wczytaj klucz enc i mac z NVS (zapisane po set_key_from_passphrase) */
    size_t enc_len = AES_KEY_BYTES, mac_len = AES_KEY_BYTES;
    err = nvs_load_blob(NVS_KEY_SALT, s_enc_key, &enc_len);
    /* UWAGA: s_enc_key i s_mac_key są ładowane ze specjalnego bloba kluczy */
    /* poniżej ładujemy właściwe klucze pochodne */
    uint8_t enc_tmp[AES_KEY_BYTES], mac_tmp[AES_KEY_BYTES];
    enc_len = AES_KEY_BYTES; mac_len = AES_KEY_BYTES;

    /* Użyj dedykowanych kluczy NVS dla enc/mac subkeys */
    err = nvs_get_blob(s_nvs, "enc_subkey", enc_tmp, &enc_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Brak enc_subkey w NVS"); return err; }
    err = nvs_get_blob(s_nvs, "mac_subkey", mac_tmp, &mac_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Brak mac_subkey w NVS"); return err; }

    memcpy(s_enc_key, enc_tmp, AES_KEY_BYTES);
    memcpy(s_mac_key, mac_tmp, AES_KEY_BYTES);

    /* Wyczyść tymczasowe kopie ze stosu */
    mbedtls_platform_zeroize(enc_tmp, AES_KEY_BYTES);
    mbedtls_platform_zeroize(mac_tmp, AES_KEY_BYTES);

    s_ready = true;
    ESP_LOGI(TAG, "Moduł kryptograficzny gotowy (AES-256-CBC + HMAC-SHA256)");
    return ESP_OK;
}

bool crypto_manager_is_ready(void) { return s_ready; }

esp_err_t crypto_manager_set_key_from_passphrase(const char *passphrase,
                                                   size_t len) {
    if (!passphrase || len < 12) {
        ESP_LOGE(TAG, "Hasło za krótkie (min 12 znaków)");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_open_crypto();
    if (err != ESP_OK) return err;

    /* Nie pozwól nadpisać raz ustawionego klucza */
    uint8_t key_ver = 0;
    nvs_get_u8(s_nvs, NVS_KEY_KEY_VER, &key_ver);
    if (key_ver != 0) {
        ESP_LOGE(TAG, "Klucz już ustawiony — factory reset wymagany do zmiany");
        return ESP_ERR_INVALID_STATE;
    }

    /* Generuj lub odczytaj sól urządzenia */
    uint8_t salt[32] = {};
    size_t salt_len = 32;
    err = nvs_get_blob(s_nvs, NVS_KEY_SALT, salt, &salt_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        derive_device_salt(salt);
        nvs_save_blob(NVS_KEY_SALT, salt, 32);
        ESP_LOGI(TAG, "Nowa sól urządzenia wygenerowana i zapisana");
    }

    /* Wyprowadź master key przez PBKDF2 */
    uint8_t master[AES_KEY_BYTES] = {};
    ESP_LOGI(TAG, "PBKDF2 (%d iteracji) — może potrwać ~1s…", PBKDF2_ITER);
    err = derive_master_key(passphrase, len, salt, master);
    if (err != ESP_OK) goto cleanup;

    /* Wyprowadź subklucze enc + mac przez HKDF */
    err = derive_subkeys(master);
    if (err != ESP_OK) goto cleanup;

    /* Zapisz subklucze do NVS */
    err = nvs_save_blob("enc_subkey", s_enc_key, AES_KEY_BYTES);
    if (err != ESP_OK) goto cleanup;
    err = nvs_save_blob("mac_subkey", s_mac_key, AES_KEY_BYTES);
    if (err != ESP_OK) goto cleanup;

    /* Ustaw flagę — od teraz klucz jest zablokowany */
    nvs_set_u8(s_nvs, NVS_KEY_KEY_VER, 1);
    nvs_commit(s_nvs);

    s_ready = true;
    ESP_LOGI(TAG, "Klucz szyfrujący ustawiony i zablokowany");

cleanup:
    mbedtls_platform_zeroize(master, AES_KEY_BYTES);
    return err;
}

/* ── Szyfrowanie ─────────────────────────────────────────────────────────── */
esp_err_t crypto_encrypt(const uint8_t *plain, size_t plain_len,
                          uint8_t *out, size_t *out_len) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (plain_len > CRYPTO_PLAIN_MAX) return ESP_ERR_INVALID_SIZE;

    /* Losowe IV */
    uint8_t iv[AES_IV_BYTES];
    esp_fill_random(iv, AES_IV_BYTES);

    /* Bufor z paddingiem */
    uint8_t padded[CRYPTO_PLAIN_MAX + 16];
    memcpy(padded, plain, plain_len);
    size_t padded_len = pkcs7_pad(padded, plain_len, 16);

    /* AES-256-CBC szyfrowanie */
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    uint8_t iv_copy[AES_IV_BYTES];
    memcpy(iv_copy, iv, AES_IV_BYTES);

    int r = mbedtls_aes_setkey_enc(&aes, s_enc_key, AES_KEY_BITS);
    if (r == 0)
        r = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT,
                                   padded_len, iv_copy,
                                   padded, padded);
    mbedtls_aes_free(&aes);
    if (r != 0) { ESP_LOGE(TAG, "AES encrypt błąd: %d", r); return ESP_FAIL; }

    /* Złóż blob: [IV][HMAC placeholder][ciphertext] */
    memcpy(out, iv, AES_IV_BYTES);
    memset(out + AES_IV_BYTES, 0, HMAC_BYTES);          /* placeholder HMAC */
    memcpy(out + AES_IV_BYTES + HMAC_BYTES, padded, padded_len);
    *out_len = AES_IV_BYTES + HMAC_BYTES + padded_len;

    /* Oblicz HMAC nad [IV || ciphertext] i wpisz na właściwe miejsce */
    uint8_t mac[HMAC_BYTES];
    esp_err_t err = compute_hmac(padded, padded_len, mac);
    /* Chcemy HMAC nad [IV || ciphertext] */
    uint8_t iv_ct[AES_IV_BYTES + CRYPTO_PLAIN_MAX + 16];
    memcpy(iv_ct, iv, AES_IV_BYTES);
    memcpy(iv_ct + AES_IV_BYTES, padded, padded_len);
    err = compute_hmac(iv_ct, AES_IV_BYTES + padded_len, mac);
    if (err != ESP_OK) return err;
    memcpy(out + AES_IV_BYTES, mac, HMAC_BYTES);

    mbedtls_platform_zeroize(padded, sizeof(padded));
    ESP_LOGD(TAG, "Zaszyfrowano %u → %u B", (unsigned)plain_len, (unsigned)*out_len);
    return ESP_OK;
}

/* ── Deszyfrowanie ───────────────────────────────────────────────────────── */
esp_err_t crypto_decrypt(const uint8_t *blob, size_t blob_len,
                          uint8_t *plain, size_t *plain_len) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (blob_len < AES_IV_BYTES + HMAC_BYTES + 16) return ESP_ERR_INVALID_SIZE;

    const uint8_t *iv       = blob;
    const uint8_t *mac_recv = blob + AES_IV_BYTES;
    const uint8_t *ct       = blob + AES_IV_BYTES + HMAC_BYTES;
    size_t         ct_len   = blob_len - AES_IV_BYTES - HMAC_BYTES;

    /* Weryfikacja HMAC */
    uint8_t iv_ct[AES_IV_BYTES + CRYPTO_PLAIN_MAX + 16];
    memcpy(iv_ct, iv, AES_IV_BYTES);
    memcpy(iv_ct + AES_IV_BYTES, ct, ct_len);

    uint8_t mac_calc[HMAC_BYTES];
    esp_err_t err = compute_hmac(iv_ct, AES_IV_BYTES + ct_len, mac_calc);
    if (err != ESP_OK) return err;

    if (!hmac_equal(mac_recv, mac_calc)) {
        ESP_LOGE(TAG, "HMAC weryfikacja NIEUDANA — dane uszkodzone lub zły klucz");
        return ESP_ERR_INVALID_CRC;
    }

    /* AES-256-CBC deszyfrowanie */
    uint8_t buf[CRYPTO_PLAIN_MAX + 16];
    memcpy(buf, ct, ct_len);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    uint8_t iv_copy[AES_IV_BYTES];
    memcpy(iv_copy, iv, AES_IV_BYTES);

    int r = mbedtls_aes_setkey_dec(&aes, s_enc_key, AES_KEY_BITS);
    if (r == 0)
        r = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT,
                                   ct_len, iv_copy, buf, buf);
    mbedtls_aes_free(&aes);

    if (r != 0) {
        ESP_LOGE(TAG, "AES decrypt błąd: %d", r);
        mbedtls_platform_zeroize(buf, sizeof(buf));
        return ESP_FAIL;
    }

    /* Usuń PKCS#7 padding */
    size_t out_len = ct_len;
    err = pkcs7_unpad(buf, &out_len, 16);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Błąd paddingu PKCS#7");
        mbedtls_platform_zeroize(buf, sizeof(buf));
        return err;
    }

    memcpy(plain, buf, out_len);
    *plain_len = out_len;
    mbedtls_platform_zeroize(buf, sizeof(buf));
    ESP_LOGD(TAG, "Odszyfrowano %u → %u B", (unsigned)blob_len, (unsigned)out_len);
    return ESP_OK;
}

/* ── Wygodne wrappersy dla struktur TUYA / SATEL ────────────────────────── */
static esp_err_t crypto_save_struct(const char *nvs_key,
                                     const void *data, size_t size) {
    uint8_t blob[CRYPTO_BLOB_MAX];
    size_t  blob_len = 0;
    esp_err_t err = crypto_encrypt((const uint8_t *)data, size,
                                    blob, &blob_len);
    if (err != ESP_OK) return err;
    return nvs_save_blob(nvs_key, blob, blob_len);
}

static esp_err_t crypto_load_struct(const char *nvs_key,
                                     void *data, size_t expected_size) {
    uint8_t blob[CRYPTO_BLOB_MAX];
    size_t  blob_len = CRYPTO_BLOB_MAX;
    esp_err_t err = nvs_load_blob(nvs_key, blob, &blob_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Brak sekretu '%s' w NVS (jeszcze nie zapisany)", nvs_key);
        return err;
    }
    uint8_t plain[CRYPTO_PLAIN_MAX];
    size_t  plain_len = 0;
    err = crypto_decrypt(blob, blob_len, plain, &plain_len);
    if (err != ESP_OK) return err;
    if (plain_len != expected_size) {
        ESP_LOGE(TAG, "Rozmiar odszyfrowanej struktury (%u) ≠ oczekiwany (%u)",
                 (unsigned)plain_len, (unsigned)expected_size);
        mbedtls_platform_zeroize(plain, sizeof(plain));
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, plain, plain_len);
    mbedtls_platform_zeroize(plain, sizeof(plain));
    return ESP_OK;
}

esp_err_t crypto_save_tuya_creds(const void *c, size_t s) {
    ESP_LOGI(TAG, "Zapis zaszyfrowanych danych TUYA");
    return crypto_save_struct(NVS_KEY_TUYA, c, s);
}

esp_err_t crypto_load_tuya_creds(void *c, size_t s) {
    return crypto_load_struct(NVS_KEY_TUYA, c, s);
}

esp_err_t crypto_save_satel_creds(const void *c, size_t s) {
    ESP_LOGI(TAG, "Zapis zaszyfrowanych danych SATEL");
    return crypto_save_struct(NVS_KEY_SATEL, c, s);
}

esp_err_t crypto_load_satel_creds(void *c, size_t s) {
    return crypto_load_struct(NVS_KEY_SATEL, c, s);
}

esp_err_t crypto_erase_secrets(void) {
    ESP_LOGW(TAG, "Kasowanie wszystkich sekretów kryptograficznych");
    nvs_erase_key(s_nvs, NVS_KEY_TUYA);
    nvs_erase_key(s_nvs, NVS_KEY_SATEL);
    nvs_erase_key(s_nvs, "enc_subkey");
    nvs_erase_key(s_nvs, "mac_subkey");
    nvs_erase_key(s_nvs, NVS_KEY_KEY_VER);
    nvs_erase_key(s_nvs, NVS_KEY_SALT);
    /* Wyczyść klucze z RAM */
    mbedtls_platform_zeroize(s_enc_key, AES_KEY_BYTES);
    mbedtls_platform_zeroize(s_mac_key, AES_KEY_BYTES);
    s_ready = false;
    return nvs_commit(s_nvs);
}
