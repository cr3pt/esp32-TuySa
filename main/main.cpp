#include "satel_client.h"
#include "tuya_client.h"
#include "crypto_manager.h"
#include "config_manager.h"
#include "system_services.h"
#include "ota_manager.h"
#include "web_tls.h"
#include "mqtt_bridge.h"
#include "webhook_client.h"
#include "event_log.h"
#include "automation_modes.h"
#include "rate_limit.h"
#include "hw_watchdog.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "MAIN";
#define DEMO_LIGHT_ID  "bfb123456789abcdef01"
#define DEMO_ZONE_NUM  1

static rule_time_window_t g_night_rule = {
    .enabled = true,
    .hour_from = 22,
    .minute_from = 0,
    .hour_to = 6,
    .minute_to = 0,
    .only_mode = MODE_NIGHT,
    .delay_ms = 0,
    .auto_off_ms = 180000
};

static void delayed_light_off_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(g_night_rule.auto_off_ms));
    tuya_cmd_bool(DEMO_LIGHT_ID, "switch_led", false);
    event_log_add(EV_INFO, "rule", "Auto-off executed for %s", DEMO_LIGHT_ID);
    vTaskDelete(NULL);
}

static void on_zone_change(int zone, bool violated, void *) {
    uint32_t now = (uint32_t)time(NULL);
    if (!rate_limit_allow("zone_cb", 20, 60)) {
        event_log_add(EV_WARN, "rate", "zone_cb rate limited");
        return;
    }

    mqtt_bridge_publish_satel_zone(zone, violated);
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"zone\":%d,\"violated\":%s}", zone, violated ? "true" : "false");
    webhook_client_post_json("/satel/zone", payload);

    if (zone == DEMO_ZONE_NUM && violated && automation_modes_match(&g_night_rule, now)) {
        tuya_cmd_bool(DEMO_LIGHT_ID, "switch_led", true);
        event_log_add(EV_SATEL, "rule", "Night rule triggered: zone %d -> light on", zone);
        xTaskCreate(delayed_light_off_task, "light_off", 3072, NULL, 4, NULL);
    }
}

static void on_dp_change(const char *dev_id, const tuya_dp_t *dp, void *) {
    char val[64] = {};
    if (dp->type == TUYA_DP_BOOL) snprintf(val, sizeof(val), "%s", dp->value.b ? "true" : "false");
    else if (dp->type == TUYA_DP_INT) snprintf(val, sizeof(val), "%ld", (long)dp->value.i);
    else snprintf(val, sizeof(val), "%s", dp->value.s);

    mqtt_bridge_publish_tuya_dp(dev_id, dp->code, val);
    char payload[256];
    snprintf(payload, sizeof(payload), "{\"device\":\"%s\",\"code\":\"%s\",\"value\":\"%s\"}", dev_id, dp->code, val);
    webhook_client_post_json("/tuya/dp", payload);
    event_log_add(EV_TUYA, "tuya", "DP %s %s=%s", dev_id, dp->code, val);

    if (strcmp(dp->code, "switch_led") == 0 && dp->type == TUYA_DP_BOOL && dp->value.b) {
        satel_ip_input_on(1);
        event_log_add(EV_INFO, "scene", "Light ON -> SATEL IP input 1 ON");
    }
}

extern "C" void app_main(void) {
    nvs_flash_init();
    event_log_init();
    rate_limit_init();
    automation_modes_init();
    automation_modes_set(MODE_NIGHT);
    hw_watchdog_init(15);
    hw_watchdog_add_current_task();

    satel_creds_t sc = {};
    strncpy(sc.host, "192.168.1.100", sizeof(sc.host)-1);
    sc.port = 7094;
    strncpy(sc.password, "1234", sizeof(sc.password)-1);
    crypto_save_satel_creds(&sc, sizeof(sc));

    tuya_creds_t tc = {};
    strncpy(tc.region, "eu", sizeof(tc.region)-1);
    strncpy(tc.client_id, "CLIENT_ID_HERE", sizeof(tc.client_id)-1);
    strncpy(tc.client_secret, "CLIENT_SECRET_HERE", sizeof(tc.client_secret)-1);
    strncpy(tc.user_uid, "USER_UID_HERE", sizeof(tc.user_uid)-1);
    crypto_save_tuya_creds(&tc, sizeof(tc));

    net_config_t nc = {};
    strncpy(nc.hostname, "esp32-bridge", sizeof(nc.hostname)-1);
    strncpy(nc.ntp_server, "pool.ntp.org", sizeof(nc.ntp_server)-1);
    config_manager_save_net(&nc);

    system_services_init(nc.hostname, nc.ntp_server);
    system_services_start();
    web_tls_init("esp32-bridge.local");
    ota_manager_init();
    char broker[128]; snprintf(broker, sizeof(broker), "mqtt://192.168.1.10"); mqtt_bridge_init(broker, "esp32/bridge");
    mqtt_bridge_start();
    mqtt_bridge_publish_ha_discovery();
    webhook_client_init("http://192.168.1.20:8080");

    http_server_init("admin", "StrongPass123!");
    http_server_start();

    satel_client_init();
    tuya_client_init();
    satel_set_zone_cb(on_zone_change, NULL);
    tuya_set_dp_change_cb(on_dp_change, NULL);
    satel_client_start();
    tuya_client_start();

    event_log_add(EV_INFO, "main", "Bridge stage 5 started");
    ESP_LOGI(TAG, "Bridge stage 5 started: scheduler + modes + security + watchdog");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        hw_watchdog_feed();
        char *m  = automation_modes_status_json();
        char *rl = rate_limit_status_json();
        char *wd = hw_watchdog_status_json();
        ESP_LOGI(TAG, "MODE: %s", m ? m : "null");
        ESP_LOGI(TAG, "RATE: %s", rl ? rl : "null");
        ESP_LOGI(TAG, "WDT: %s", wd ? wd : "null");
        free(m); free(rl); free(wd);
    }
}
