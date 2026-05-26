#include "mqtt_bridge.h"
#include "event_log.h"
#include "esp_mqtt_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "MQTT";
static esp_mqtt_client_handle_t s_client = NULL;
static mqtt_bridge_status_t s_status = {};

static void publish(const char *topic, const char *payload, bool retain) {
    if (!s_client) return;
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, retain ? 1 : 0);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)handler_args; (void)base; (void)event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        s_status.connected = true;
        event_log_add(EV_MQTT, "mqtt", "Connected to broker %s", s_status.broker);
        ESP_LOGI(TAG, "MQTT connected");
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        s_status.connected = false;
        s_status.reconnects++;
        event_log_add(EV_WARN, "mqtt", "Disconnected from broker");
        ESP_LOGW(TAG, "MQTT disconnected");
    }
}

esp_err_t mqtt_bridge_init(const char *broker_url, const char *base_topic) {
    memset(&s_status, 0, sizeof(s_status));
    snprintf(s_status.broker, sizeof(s_status.broker), "%s", broker_url ? broker_url : "mqtt://192.168.1.10");
    snprintf(s_status.base_topic, sizeof(s_status.base_topic), "%s", base_topic ? base_topic : "esp32/bridge");

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = s_status.broker;
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    return ESP_OK;
}

esp_err_t mqtt_bridge_start(void) {
    return s_client ? esp_mqtt_client_start(s_client) : ESP_ERR_INVALID_STATE;
}

esp_err_t mqtt_bridge_publish_json(const char *topic_suffix, const char *json, bool retain) {
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/%s", s_status.base_topic, topic_suffix ? topic_suffix : "state");
    publish(topic, json ? json : "{}", retain);
    return ESP_OK;
}

esp_err_t mqtt_bridge_publish_event(const char *source, const char *message) {
    char topic[160], payload[256];
    snprintf(topic, sizeof(topic), "%s/events/%s", s_status.base_topic, source ? source : "sys");
    snprintf(payload, sizeof(payload), "{\"source\":\"%s\",\"message\":\"%s\"}", source ? source : "sys", message ? message : "");
    publish(topic, payload, false);
    return ESP_OK;
}

esp_err_t mqtt_bridge_publish_satel_zone(int zone, bool violated) {
    char suffix[64], payload[128];
    snprintf(suffix, sizeof(suffix), "satel/zone/%d", zone);
    snprintf(payload, sizeof(payload), "{\"zone\":%d,\"violated\":%s}", zone, violated ? "true" : "false");
    return mqtt_bridge_publish_json(suffix, payload, true);
}

esp_err_t mqtt_bridge_publish_tuya_dp(const char *dev_id, const char *code, const char *value) {
    char suffix[128], payload[256];
    snprintf(suffix, sizeof(suffix), "tuya/%s/%s", dev_id ? dev_id : "unknown", code ? code : "dp");
    snprintf(payload, sizeof(payload), "{\"device\":\"%s\",\"code\":\"%s\",\"value\":\"%s\"}", dev_id ? dev_id : "", code ? code : "", value ? value : "");
    return mqtt_bridge_publish_json(suffix, payload, true);
}

esp_err_t mqtt_bridge_publish_ha_discovery(void) {
    char topic[256], payload[512];
    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/esp32_bridge_satel_zone1/config");
    snprintf(payload, sizeof(payload),
        "{\"name\":\"Satel Zone 1\",\"unique_id\":\"esp32_bridge_zone1\",\"state_topic\":\"%s/satel/zone/1\",\"value_template\":\"{{ value_json.violated }}\",\"payload_on\":true,\"payload_off\":false}",
        s_status.base_topic);
    publish(topic, payload, true);
    s_status.ha_discovery_sent = true;
    event_log_add(EV_MQTT, "mqtt", "Home Assistant discovery published");
    return ESP_OK;
}

const mqtt_bridge_status_t *mqtt_bridge_get_status(void) { return &s_status; }
char *mqtt_bridge_status_json(void) {
    char *buf = (char*)malloc(256);
    if (!buf) return NULL;
    snprintf(buf, 256,
        "{\"connected\":%s,\"reconnects\":%d,\"broker\":\"%s\",\"base_topic\":\"%s\",\"ha_discovery\":%s}",
        s_status.connected ? "true" : "false", s_status.reconnects, s_status.broker, s_status.base_topic,
        s_status.ha_discovery_sent ? "true" : "false");
    return buf;
}
