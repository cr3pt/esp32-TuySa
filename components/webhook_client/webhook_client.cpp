#include "webhook_client.h"
#include "event_log.h"
#include "esp_http_client.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static webhook_status_t s_status = {};
static char s_base_url[160] = {};

esp_err_t webhook_client_init(const char *base_url) {
    memset(&s_status, 0, sizeof(s_status));
    snprintf(s_base_url, sizeof(s_base_url), "%s", base_url ? base_url : "http://127.0.0.1:8080");
    s_status.enabled = true;
    return ESP_OK;
}

esp_err_t webhook_client_post_json(const char *path, const char *json) {
    if (!s_status.enabled) return ESP_ERR_INVALID_STATE;
    char url[256];
    snprintf(url, sizeof(url), "%s%s", s_base_url, path ? path : "/event");
    snprintf(s_status.last_url, sizeof(s_status.last_url), "%s", url);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 5000;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return ESP_FAIL;
    esp_http_client_set_method(h, HTTP_METHOD_POST);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    if (json) esp_http_client_set_post_field(h, json, strlen(json));
    esp_err_t err = esp_http_client_perform(h);
    esp_http_client_cleanup(h);

    if (err == ESP_OK) {
        s_status.sent_ok++;
        event_log_add(EV_WEBHOOK, "webhook", "POST %s ok", url);
    } else {
        s_status.sent_fail++;
        event_log_add(EV_ERROR, "webhook", "POST %s fail=%d", url, err);
    }
    return err;
}

const webhook_status_t *webhook_client_get_status(void) { return &s_status; }
char *webhook_client_status_json(void) {
    char *buf = (char*)malloc(256);
    if (!buf) return NULL;
    snprintf(buf, 256,
        "{\"enabled\":%s,\"sent_ok\":%d,\"sent_fail\":%d,\"last_url\":\"%s\"}",
        s_status.enabled ? "true" : "false", s_status.sent_ok, s_status.sent_fail, s_status.last_url);
    return buf;
}
