#include "config_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG       = "CFG";
static const char *NVS_NS    = "bridge_cfg";   /* namespace NVS */

/* Klucze NVS */
#define KEY_PROVISIONED "provisioned"
#define KEY_NET         "net_blob"
#define KEY_TUYA        "tuya_blob"
#define KEY_SATEL       "satel_blob"
#define KEY_ENC_KEY     "enc_key"

static nvs_handle_t s_nvs = 0;

/* ── Pomocnicze: zapis / odczyt blobu ───────────────────────────────────── */
static esp_err_t nvs_write_blob(const char *key, const void *data, size_t len) {
    esp_err_t err = nvs_set_blob(s_nvs, key, data, len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_set_blob(%s) err=0x%x", key, err); return err; }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_commit err=0x%x", err); }
    return err;
}

static esp_err_t nvs_read_blob(const char *key, void *data, size_t expected) {
    size_t len = expected;
    esp_err_t err = nvs_get_blob(s_nvs, key, data, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "nvs_get_blob(%s) – brak klucza", key);
        return err;
    }
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_get_blob(%s) err=0x%x", key, err); }
    return err;
}

/* ── API ──────────────────────────────────────────────────────────────────── */
esp_err_t config_manager_init(void) {
    if (s_nvs != 0) return ESP_OK;   /* już otwarte */
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open() err=0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "Przestrzeń NVS '%s' otwarta", NVS_NS);
    return ESP_OK;
}

bool config_manager_is_provisioned(void) {
    uint8_t flag = 0;
    esp_err_t err = nvs_get_u8(s_nvs, KEY_PROVISIONED, &flag);
    return (err == ESP_OK && flag == 1);
}

/* ── Sieć ──────────────────────────────────────────────────────────────── */
esp_err_t config_manager_save_net(const net_config_t *cfg) {
    esp_err_t err = nvs_write_blob(KEY_NET, cfg, sizeof(net_config_t));
    if (err != ESP_OK) return err;
    /* Ustaw flagę provisioned dopiero po pomyślnym zapisie */
    err = nvs_set_u8(s_nvs, KEY_PROVISIONED, 1);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    ESP_LOGI(TAG, "Konfiguracja sieciowa zapisana, SSID='%s'", cfg->ssid);
    return err;
}

esp_err_t config_manager_load_net(net_config_t *cfg) {
    return nvs_read_blob(KEY_NET, cfg, sizeof(net_config_t));
}

/* ── TUYA ──────────────────────────────────────────────────────────────── */
esp_err_t config_manager_save_tuya(const tuya_creds_t *creds) {
    ESP_LOGI(TAG, "Zapis poświadczeń TUYA (client_id='%s')", creds->client_id);
    return nvs_write_blob(KEY_TUYA, creds, sizeof(tuya_creds_t));
}

esp_err_t config_manager_load_tuya(tuya_creds_t *creds) {
    return nvs_read_blob(KEY_TUYA, creds, sizeof(tuya_creds_t));
}

/* ── SATEL ─────────────────────────────────────────────────────────────── */
esp_err_t config_manager_save_satel(const satel_creds_t *creds) {
    ESP_LOGI(TAG, "Zapis poświadczeń SATEL (host='%s':%u)", creds->host, creds->port);
    return nvs_write_blob(KEY_SATEL, creds, sizeof(satel_creds_t));
}

esp_err_t config_manager_load_satel(satel_creds_t *creds) {
    return nvs_read_blob(KEY_SATEL, creds, sizeof(satel_creds_t));
}

/* ── Klucz szyfrujący ──────────────────────────────────────────────────── */
esp_err_t config_manager_save_enc_key(const uint8_t key[CFG_ENC_KEY_LEN]) {
    ESP_LOGI(TAG, "Zapis klucza szyfrującego urządzenia");
    return nvs_write_blob(KEY_ENC_KEY, key, CFG_ENC_KEY_LEN);
}

esp_err_t config_manager_load_enc_key(uint8_t key[CFG_ENC_KEY_LEN]) {
    return nvs_read_blob(KEY_ENC_KEY, key, CFG_ENC_KEY_LEN);
}

/* ── Factory reset ─────────────────────────────────────────────────────── */
esp_err_t config_manager_erase_all(void) {
    ESP_LOGW(TAG, "Kasowanie całej konfiguracji (factory reset)");
    esp_err_t err = nvs_erase_all(s_nvs);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs);
}
