#include "system_services.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "mdns.h"
#include "esp_netif_sntp.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "SYS_SVC";
static system_services_status_t s_status = {};

static void time_sync_cb(struct timeval *tv) {
    (void)tv;
    s_status.ntp_synced = true;
    s_status.sync_epoch = (uint32_t)time(NULL);
    ESP_LOGI(TAG, "NTP synchronized: %lu", (unsigned long)s_status.sync_epoch);
}

esp_err_t system_services_init(const char *hostname, const char *ntp_server) {
    memset(&s_status, 0, sizeof(s_status));
    strncpy(s_status.hostname, hostname && *hostname ? hostname : "esp32-bridge", sizeof(s_status.hostname)-1);
    strncpy(s_status.ntp_server, ntp_server && *ntp_server ? ntp_server : "pool.ntp.org", sizeof(s_status.ntp_server)-1);
    return ESP_OK;
}

esp_err_t system_services_start(void) {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_status.ntp_server);
    config.sync_cb = time_sync_cb;
    esp_netif_sntp_init(&config);
    if (esp_netif_sntp_start() == ESP_OK) {
        ESP_LOGI(TAG, "NTP started using %s", s_status.ntp_server);
    }

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_status.hostname);
        mdns_instance_name_set("ESP32 TUYA SATEL Bridge");
        mdns_service_add("Bridge UI", "_http", "_tcp", 80, NULL, 0);
        s_status.mdns_started = true;
        ESP_LOGI(TAG, "mDNS started: http://%s.local", s_status.hostname);
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }
    return ESP_OK;
}

esp_err_t system_services_stop(void) {
    esp_netif_sntp_deinit();
    mdns_free();
    s_status.mdns_started = false;
    return ESP_OK;
}

bool system_services_ntp_synced(void) { return s_status.ntp_synced; }
uint32_t system_services_get_epoch(void) { return (uint32_t)time(NULL); }
const system_services_status_t *system_services_get_status(void) { return &s_status; }

char *system_services_status_json(void) {
    char *buf = (char*)malloc(256);
    if (!buf) return NULL;
    snprintf(buf, 256,
        "{\"ntp_synced\":%s,\"mdns_started\":%s,\"hostname\":\"%s\",\"ntp_server\":\"%s\",\"epoch\":%lu}",
        s_status.ntp_synced ? "true" : "false",
        s_status.mdns_started ? "true" : "false",
        s_status.hostname,
        s_status.ntp_server,
        (unsigned long)system_services_get_epoch());
    return buf;
}
