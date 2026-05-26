#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool connected;
    int reconnects;
    char broker[96];
    char base_topic[64];
    bool ha_discovery_sent;
} mqtt_bridge_status_t;

esp_err_t mqtt_bridge_init(const char *broker_url, const char *base_topic);
esp_err_t mqtt_bridge_start(void);
esp_err_t mqtt_bridge_publish_json(const char *topic_suffix, const char *json, bool retain);
esp_err_t mqtt_bridge_publish_event(const char *source, const char *message);
esp_err_t mqtt_bridge_publish_satel_zone(int zone, bool violated);
esp_err_t mqtt_bridge_publish_tuya_dp(const char *dev_id, const char *code, const char *value);
esp_err_t mqtt_bridge_publish_ha_discovery(void);
const mqtt_bridge_status_t *mqtt_bridge_get_status(void);
char *mqtt_bridge_status_json(void);

#ifdef __cplusplus
}
#endif
