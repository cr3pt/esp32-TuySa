#include "ota_manager.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "OTA";
static ota_status_t s_ota = {};

esp_err_t ota_manager_init(void) {
    memset(&s_ota, 0, sizeof(s_ota));
    return ESP_OK;
}

esp_err_t ota_manager_https_url(const char *url) {
    if (!url || !*url) return ESP_ERR_INVALID_ARG;
    s_ota.running = true;
    s_ota.last_ok = false;
    strncpy(s_ota.last_url, url, sizeof(s_ota.last_url)-1);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.timeout_ms = 15000;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;

    ESP_LOGI(TAG, "Starting OTA from %s", url);
    esp_err_t err = esp_https_ota(&ota_cfg);
    s_ota.last_status = err;
    s_ota.running = false;
    s_ota.last_ok = (err == ESP_OK);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA success, rebooting...");
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA failed: %d", err);
    return err;
}

esp_err_t ota_manager_mark_valid(void) {
    return esp_ota_mark_app_valid_cancel_rollback();
}

const ota_status_t *ota_manager_get_status(void) { return &s_ota; }

char *ota_manager_status_json(void) {
    char *buf = (char*)malloc(320);
    if (!buf) return NULL;
    snprintf(buf, 320,
        "{\"running\":%s,\"last_ok\":%s,\"last_status\":%d,\"last_url\":\"%s\"}",
        s_ota.running ? "true" : "false",
        s_ota.last_ok ? "true" : "false",
        s_ota.last_status,
        s_ota.last_url);
    return buf;
}
