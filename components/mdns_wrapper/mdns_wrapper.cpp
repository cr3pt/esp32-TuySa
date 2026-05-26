#include "mdns_wrapper.h"
#include "esp_log.h"
#include "esp_mdns.h"

static const char *TAG = "mdns_wrapper";

esp_err_t mdns_wrapper_init(const char *hostname) {
    esp_err_t err = esp_mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (hostname) {
        err = esp_mdns_set_hostname(hostname);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_mdns_set_hostname failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "mDNS hostname set to: %s.local", hostname);
    }
    return ESP_OK;
}

esp_err_t mdns_wrapper_set_hostname(const char *hostname) {
    if (!hostname) return ESP_ERR_INVALID_ARG;
    return esp_mdns_set_hostname(hostname);
}
