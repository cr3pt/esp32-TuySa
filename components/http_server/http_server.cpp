#include "http_server.h"
#include "config_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_fill_random.h"
#include "mbedtls/sha256.h"
#include "system_services.h"
#include "web_tls.h"
#include "ota_manager.h"
#include "automation_modes.h"
#include "rate_limit.h"
#include "hw_watchdog.h"
#include "event_log.h"
#include "mqtt_bridge.h"
#include "webhook_client.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

static const char *TAG = "HTTP";
static httpd_handle_t s_server = NULL;
static http_server_status_t s_status = {};
static panel_auth_t s_auth = {};

static const char INDEX_HTML[] = "<!doctype html><html><body><h1>ESP32 Bridge API</h1><p>HTTP API secured with Basic Auth + SHA-256(salt||password).</p></body></html>";
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(char c){ const char *p=strchr(B64,c); return p?(int)(p-B64):-1; }
static void b64_encode(const uint8_t *src, size_t len, char *out, size_t outlen) {
    size_t i=0,o=0; while (i < len && o+4 < outlen) {
        size_t rem = len - i;
        uint32_t a = src[i++];
        uint32_t b = rem > 1 ? src[i++] : 0;
        uint32_t c = rem > 2 ? src[i++] : 0;
        uint32_t triple = (a<<16)|(b<<8)|c;
        out[o++] = B64[(triple>>18)&0x3F];
        out[o++] = B64[(triple>>12)&0x3F];
        out[o++] = rem > 1 ? B64[(triple>>6)&0x3F] : '=';
        out[o++] = rem > 2 ? B64[triple&0x3F] : '=';
    }
    out[o]=0;
}
static int b64_decode(const char *in, uint8_t *out, size_t outlen) {
    size_t len=strlen(in), i=0, o=0;
    while (i < len) {
        int a=b64val(in[i++]); if (a<0) break;
        int b=b64val(in[i++]); if (b<0) break;
        int c=in[i]=='='?-1:b64val(in[i]); i++;
        int d=in[i]=='='?-1:b64val(in[i]); i++;
        uint32_t t=(a<<18)|(b<<12)|((c<0?0:c)<<6)|((d<0?0:d));
        if (o<outlen) out[o++]=(t>>16)&0xFF;
        if (c>=0 && o<outlen) out[o++]=(t>>8)&0xFF;
        if (d>=0 && o<outlen) out[o++]=t&0xFF;
    }
    if (o<outlen) out[o]=0;
    return (int)o;
}
static void sha256_salted(const uint8_t salt[16], const char *password, uint8_t out[32]) {
    mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx); mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt, 16);
    mbedtls_sha256_update(&ctx, (const unsigned char*)password, strlen(password));
    mbedtls_sha256_finish(&ctx, out); mbedtls_sha256_free(&ctx);
}
static bool verify_password(const char *password) {
    uint8_t h[32]; sha256_salted(s_auth.salt, password, h); return memcmp(h, s_auth.hash, 32) == 0;
}
static bool parse_basic_auth(httpd_req_t *req, char *user, size_t ulen, char *pass, size_t plen) {
    char hdr[256] = {}; if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;
    uint8_t dec[160] = {}; int n = b64_decode(hdr + 6, dec, sizeof(dec)-1); if (n <= 0) return false;
    char *sep = strchr((char*)dec, ':'); if (!sep) return false; *sep = 0;
    snprintf(user, ulen, "%s", (char*)dec); snprintf(pass, plen, "%s", sep+1); return true;
}
static bool is_auth_ok(httpd_req_t *req) {
    if (!s_status.auth_enabled) return true;
    char user[32]={}, pass[80]={}; if (!parse_basic_auth(req, user, sizeof(user), pass, sizeof(pass))) return false;
    if (strcmp(user, s_auth.username) != 0) return false;
    return verify_password(pass);
}
static esp_err_t require_auth(httpd_req_t *req) { if (is_auth_ok(req)) return ESP_OK; httpd_resp_set_status(req, "401 Unauthorized"); httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 Bridge\""); httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN); return ESP_FAIL; }
static esp_err_t json_send(httpd_req_t *req, char *buf) { httpd_resp_set_type(req, "application/json"); esp_err_t e = httpd_resp_sendstr(req, buf ? buf : "{}"); free(buf); return e; }
static esp_err_t root_get(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; httpd_resp_set_type(req, "text/html; charset=utf-8"); return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN); }
static esp_err_t status_get(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; return json_send(req, http_server_status_json()); }
static esp_err_t system_get(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; return json_send(req, system_services_status_json()); }
static esp_err_t tls_get(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; return json_send(req, web_tls_status_json()); }
static esp_err_t ota_get(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; return json_send(req, ota_manager_status_json()); }
static esp_err_t mode_get(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; return json_send(req, automation_modes_status_json()); }
static esp_err_t rate_get(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; return json_send(req, rate_limit_status_json()); }
static esp_err_t watchdog_get(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; return json_send(req, hw_watchdog_status_json()); }
static esp_err_t events_get(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; return json_send(req, event_log_to_json()); }
static esp_err_t mqtt_get(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; return json_send(req, mqtt_bridge_status_json()); }
static esp_err_t webhook_get(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; return json_send(req, webhook_client_status_json()); }
static esp_err_t ota_post(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; if (!rate_limit_allow("api_ota", 3, 300)) return httpd_resp_send_err(req, HTTPD_429_TOO_MANY_REQUESTS, "rate limit"); char body[256] = {}; int r = httpd_req_recv(req, body, sizeof(body)-1); if (r <= 0) return httpd_resp_send_500(req); char *p = strstr(body, "\"url\":\""); if (!p) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url"); p += 7; char *e = strchr(p, '"'); if (!e) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad url"); *e = 0; ota_manager_https_url(p); return httpd_resp_sendstr(req, "{\"ok\":true}"); }
static esp_err_t mode_post(httpd_req_t *req) { if (require_auth(req) != ESP_OK) return ESP_FAIL; char body[128] = {}; int r = httpd_req_recv(req, body, sizeof(body)-1); if (r <= 0) return httpd_resp_send_500(req); if (strstr(body, "HOME")) automation_modes_set(MODE_HOME); else if (strstr(body, "AWAY")) automation_modes_set(MODE_AWAY); else if (strstr(body, "NIGHT")) automation_modes_set(MODE_NIGHT); else automation_modes_set(MODE_MANUAL); return httpd_resp_sendstr(req, "{\"ok\":true}"); }
static esp_err_t auth_post(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    if (!rate_limit_allow("api_auth_change", 5, 600)) return httpd_resp_send_err(req, HTTPD_429_TOO_MANY_REQUESTS, "rate limit");
    char body[256] = {}; int r = httpd_req_recv(req, body, sizeof(body)-1); if (r <= 0) return httpd_resp_send_500(req);
    char user[32] = {}, pass[64] = {}; char *u = strstr(body, "\"username\":\""); char *p = strstr(body, "\"password\":\"");
    if (!u || !p) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing username/password");
    u += 12; p += 12; char *ue = strchr(u, '"'); char *pe = strchr(p, '"'); if (!ue || !pe) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    *ue = 0; *pe = 0; snprintf(user, sizeof(user), "%s", u); snprintf(pass, sizeof(pass), "%s", p);
    if (strlen(user) < 3 || strlen(pass) < 8) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "weak credentials");
    memset(&s_auth, 0, sizeof(s_auth)); snprintf(s_auth.username, sizeof(s_auth.username), "%s", user); esp_fill_random(s_auth.salt, sizeof(s_auth.salt)); sha256_salted(s_auth.salt, pass, s_auth.hash); config_manager_save_panel_auth(&s_auth); snprintf(s_status.username, sizeof(s_status.username), "%s", s_auth.username); event_log_add(EV_INFO, "http", "Panel credentials changed for user %s", s_auth.username); return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"credentials updated\"}");
}

esp_err_t http_server_init(const char *username, const char *password) {
    memset(&s_status, 0, sizeof(s_status)); memset(&s_auth, 0, sizeof(s_auth));
    if (config_manager_load_panel_auth(&s_auth) != ESP_OK || !s_auth.username[0]) {
        snprintf(s_auth.username, sizeof(s_auth.username), "%s", username ? username : "admin");
        esp_fill_random(s_auth.salt, sizeof(s_auth.salt));
        sha256_salted(s_auth.salt, password ? password : "StrongPass123!", s_auth.hash);
        config_manager_save_panel_auth(&s_auth);
    }
    snprintf(s_status.username, sizeof(s_status.username), "%s", s_auth.username);
    s_status.auth_enabled = true;
    return ESP_OK;
}

esp_err_t http_server_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); cfg.server_port = 80; if (httpd_start(&s_server, &cfg) != ESP_OK) return ESP_FAIL; s_status.running = true;
    httpd_uri_t uris[] = {
        {.uri="/", .method=HTTP_GET, .handler=root_get}, {.uri="/api/status", .method=HTTP_GET, .handler=status_get}, {.uri="/api/system", .method=HTTP_GET, .handler=system_get}, {.uri="/api/tls", .method=HTTP_GET, .handler=tls_get}, {.uri="/api/ota", .method=HTTP_GET, .handler=ota_get}, {.uri="/api/ota", .method=HTTP_POST, .handler=ota_post}, {.uri="/api/mode", .method=HTTP_GET, .handler=mode_get}, {.uri="/api/mode", .method=HTTP_POST, .handler=mode_post}, {.uri="/api/rate-limit", .method=HTTP_GET, .handler=rate_get}, {.uri="/api/watchdog", .method=HTTP_GET, .handler=watchdog_get}, {.uri="/api/events", .method=HTTP_GET, .handler=events_get}, {.uri="/api/mqtt", .method=HTTP_GET, .handler=mqtt_get}, {.uri="/api/webhook", .method=HTTP_GET, .handler=webhook_get}, {.uri="/api/auth", .method=HTTP_POST, .handler=auth_post},
    };
    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);
    ESP_LOGI(TAG, "HTTP server started on :80 with Basic Auth + salted SHA-256");
    return ESP_OK;
}
esp_err_t http_server_stop(void) { if (s_server) httpd_stop(s_server); s_server = NULL; s_status.running = false; return ESP_OK; }
void http_server_push_event(const char *event, const char *data) { ESP_LOGI(TAG, "event: %s %s", event ? event : "", data ? data : ""); }
const http_server_status_t *http_server_get_status(void) { return &s_status; }
char *http_server_status_json(void) { char *buf = (char*)malloc(160); if (!buf) return NULL; snprintf(buf, 160, "{\"running\":%s,\"auth_enabled\":%s,\"username\":\"%s\"}", s_status.running?"true":"false", s_status.auth_enabled?"true":"false", s_status.username); return buf; }
