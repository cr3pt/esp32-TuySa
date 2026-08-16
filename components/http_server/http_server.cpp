#include "nvs_flash.h"
#include "http_server.h"
#include "config_manager.h"
#include "crypto_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_fill_random.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
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

static esp_err_t mqtt_set(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    if (!rate_limit_allow("api_mqtt_set", 5, 600)) return httpd_resp_send_err(req, HTTPD_429_TOO_MANY_REQUESTS, "rate limit");
    char body[256] = {}; int r = httpd_req_recv(req, body, sizeof(body)-1); if (r <= 0) return httpd_resp_send_500(req);
    cJSON *j = cJSON_Parse(body); if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    cJSON *bj = cJSON_GetObjectItemCaseSensitive(j, "broker"); cJSON *tj = cJSON_GetObjectItemCaseSensitive(j, "topic");
    if (!cJSON_IsString(bj) || !cJSON_IsString(tj)) { cJSON_Delete(j); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing broker/topic"); }
    char broker[128] = {}, topic[128] = {};
    snprintf(broker, sizeof(broker), "%s", bj->valuestring); snprintf(topic, sizeof(topic), "%s", tj->valuestring);
    cJSON_Delete(j);
    if (strlen(broker) < 5 || strlen(topic) < 3) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid values");
    mqtt_config_t mc = {}; snprintf(mc.broker, sizeof(mc.broker), "%s", broker); snprintf(mc.base_topic, sizeof(mc.base_topic), "%s", topic);
    config_manager_save_mqtt(&mc);
    event_log_add(EV_INFO, "http", "MQTT config updated: %s %s", broker, topic);
    return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"MQTT config updated\"}");
}

/* --- Pomocnicze do kreatora: JSON in/out (cJSON) --- */
static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buflen) {
    int r = httpd_req_recv(req, buf, buflen - 1);
    if (r <= 0) return ESP_FAIL;
    buf[r] = 0;
    return ESP_OK;
}
static char *read_body_alloc(httpd_req_t *req, size_t cap) {
    char *buf = (char*)malloc(cap);
    if (!buf) return NULL;
    int r = httpd_req_recv(req, buf, cap - 1);
    if (r <= 0) { free(buf); return NULL; }
    buf[r] = 0;
    return buf;
}
static void jstr_apply(cJSON *j, const char *key, char *dst, size_t dstlen) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
    if (it && cJSON_IsString(it) && it->valuestring) snprintf(dst, dstlen, "%s", it->valuestring);
}
/* jak jstr_apply, ale pusty string nie nadpisuje juz zapisanego sekretu (haslo/klucz) */
static void jstr_apply_secret(cJSON *j, const char *key, char *dst, size_t dstlen) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
    if (it && cJSON_IsString(it) && it->valuestring && it->valuestring[0]) snprintf(dst, dstlen, "%s", it->valuestring);
}
static void jint_apply(cJSON *j, const char *key, int *dst) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
    if (it && cJSON_IsNumber(it)) *dst = it->valueint;
}
static void jbool_apply(cJSON *j, const char *key, bool *dst) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
    if (it && cJSON_IsBool(it)) *dst = cJSON_IsTrue(it);
}

/* --- Krok kreatora: Siec --- */
static esp_err_t net_config_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    net_config_t nc = {}; config_manager_load_net(&nc);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", nc.ssid);
    cJSON_AddBoolToObject(o, "has_password", nc.password[0] != 0);
    cJSON_AddStringToObject(o, "hostname", nc.hostname);
    cJSON_AddBoolToObject(o, "dhcp", nc.dhcp);
    cJSON_AddStringToObject(o, "ip", nc.ip);
    cJSON_AddStringToObject(o, "gw", nc.gw);
    cJSON_AddStringToObject(o, "mask", nc.mask);
    cJSON_AddStringToObject(o, "dns1", nc.dns1);
    cJSON_AddStringToObject(o, "dns2", nc.dns2);
    cJSON_AddStringToObject(o, "ntp_server", nc.ntp_server);
    char *out = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    return json_send(req, out);
}
static esp_err_t net_config_post(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    if (!rate_limit_allow("api_net_set", 10, 600)) return httpd_resp_send_err(req, HTTPD_429_TOO_MANY_REQUESTS, "rate limit");
    char body[512] = {}; if (read_body(req, body, sizeof(body)) != ESP_OK) return httpd_resp_send_500(req);
    cJSON *j = cJSON_Parse(body); if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    net_config_t nc = {}; config_manager_load_net(&nc);
    jstr_apply(j, "ssid", nc.ssid, sizeof(nc.ssid));
    jstr_apply_secret(j, "password", nc.password, sizeof(nc.password));
    jstr_apply(j, "hostname", nc.hostname, sizeof(nc.hostname));
    bool dhcp = nc.dhcp; jbool_apply(j, "dhcp", &dhcp); nc.dhcp = dhcp;
    jstr_apply(j, "ip", nc.ip, sizeof(nc.ip));
    jstr_apply(j, "gw", nc.gw, sizeof(nc.gw));
    jstr_apply(j, "mask", nc.mask, sizeof(nc.mask));
    jstr_apply(j, "dns1", nc.dns1, sizeof(nc.dns1));
    jstr_apply(j, "dns2", nc.dns2, sizeof(nc.dns2));
    jstr_apply(j, "ntp_server", nc.ntp_server, sizeof(nc.ntp_server));
    cJSON_Delete(j);
    config_manager_save_net(&nc);
    event_log_add(EV_INFO, "http", "Konfiguracja sieci zapisana (kreator)");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* --- Krok kreatora: TUYA --- */
static esp_err_t tuya_config_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    tuya_creds_t tc = {}; crypto_load_tuya_creds(&tc, sizeof(tc));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "region", tc.region);
    cJSON_AddStringToObject(o, "client_id", tc.client_id);
    cJSON_AddStringToObject(o, "user_uid", tc.user_uid);
    cJSON_AddBoolToObject(o, "has_secret", tc.client_secret[0] != 0);
    char *out = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    return json_send(req, out);
}
static esp_err_t tuya_config_post(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    if (!rate_limit_allow("api_tuya_set", 10, 600)) return httpd_resp_send_err(req, HTTPD_429_TOO_MANY_REQUESTS, "rate limit");
    char body[512] = {}; if (read_body(req, body, sizeof(body)) != ESP_OK) return httpd_resp_send_500(req);
    cJSON *j = cJSON_Parse(body); if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    tuya_creds_t tc = {}; crypto_load_tuya_creds(&tc, sizeof(tc));
    jstr_apply(j, "region", tc.region, sizeof(tc.region));
    jstr_apply(j, "client_id", tc.client_id, sizeof(tc.client_id));
    jstr_apply_secret(j, "client_secret", tc.client_secret, sizeof(tc.client_secret));
    jstr_apply(j, "user_uid", tc.user_uid, sizeof(tc.user_uid));
    cJSON_Delete(j);
    crypto_save_tuya_creds(&tc, sizeof(tc));
    event_log_add(EV_INFO, "http", "Konfiguracja TUYA zapisana (kreator)");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* --- Krok kreatora: SATEL. Uwaga: protokol ETHM-1 PLUS jest tu wysylany jawnie po TCP -
   przesylany jest tylko 8-bajtowy kod uzytkownika/operatora do komend uprzywilejowanych,
   ramki NIE sa szyfrowane AES kluczem integracyjnym modulu (patrz satel_protocol.cpp). */
static esp_err_t satel_config_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    satel_creds_t sc = {}; crypto_load_satel_creds(&sc, sizeof(sc));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "host", sc.host);
    cJSON_AddNumberToObject(o, "port", sc.port);
    cJSON_AddStringToObject(o, "panel_id", sc.panel_id);
    cJSON_AddBoolToObject(o, "has_password", sc.password[0] != 0);
    char *out = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    return json_send(req, out);
}
static esp_err_t satel_config_post(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    if (!rate_limit_allow("api_satel_set", 10, 600)) return httpd_resp_send_err(req, HTTPD_429_TOO_MANY_REQUESTS, "rate limit");
    char body[256] = {}; if (read_body(req, body, sizeof(body)) != ESP_OK) return httpd_resp_send_500(req);
    cJSON *j = cJSON_Parse(body); if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    satel_creds_t sc = {}; crypto_load_satel_creds(&sc, sizeof(sc));
    jstr_apply(j, "host", sc.host, sizeof(sc.host));
    int port = sc.port; jint_apply(j, "port", &port); sc.port = (uint16_t)port;
    jstr_apply_secret(j, "password", sc.password, sizeof(sc.password));
    jstr_apply(j, "panel_id", sc.panel_id, sizeof(sc.panel_id));
    cJSON_Delete(j);
    crypto_save_satel_creds(&sc, sizeof(sc));
    event_log_add(EV_INFO, "http", "Konfiguracja SATEL zapisana (kreator)");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* --- Krok kreatora: Webhook --- */
static esp_err_t webhook_config_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    webhook_config_t wc = {}; config_manager_load_webhook(&wc);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "base_url", wc.base_url);
    char *out = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    return json_send(req, out);
}
static esp_err_t webhook_config_post(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    if (!rate_limit_allow("api_webhook_set", 10, 600)) return httpd_resp_send_err(req, HTTPD_429_TOO_MANY_REQUESTS, "rate limit");
    char body[256] = {}; if (read_body(req, body, sizeof(body)) != ESP_OK) return httpd_resp_send_500(req);
    cJSON *j = cJSON_Parse(body); if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    webhook_config_t wc = {}; config_manager_load_webhook(&wc);
    jstr_apply(j, "base_url", wc.base_url, sizeof(wc.base_url));
    cJSON_Delete(j);
    config_manager_save_webhook(&wc);
    webhook_client_init(wc.base_url);
    event_log_add(EV_INFO, "http", "Konfiguracja webhook zapisana (kreator): %s", wc.base_url);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* --- Krok kreatora: Sceny --- */
static esp_err_t scenes_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    scenes_config_t sc = {}; config_manager_load_scenes(&sc);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < sc.count && i < MAX_SCENES; i++) {
        scene_t *s = &sc.items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s->name);
        cJSON_AddBoolToObject(o, "enabled", s->enabled);
        cJSON *acts = cJSON_CreateArray();
        for (int a = 0; a < s->action_count && a < MAX_SCENE_ACTIONS; a++) {
            scene_action_t *ac = &s->actions[a];
            cJSON *ao = cJSON_CreateObject();
            cJSON_AddStringToObject(ao, "code", ac->code);
            cJSON_AddStringToObject(ao, "target", ac->target);
            cJSON_AddStringToObject(ao, "param", ac->param);
            cJSON_AddStringToObject(ao, "value", ac->value);
            cJSON_AddItemToArray(acts, ao);
        }
        cJSON_AddItemToObject(o, "actions", acts);
        cJSON_AddItemToArray(arr, o);
    }
    char *out = cJSON_PrintUnformatted(arr); cJSON_Delete(arr);
    return json_send(req, out);
}
static esp_err_t scenes_post(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    if (!rate_limit_allow("api_scenes_set", 10, 600)) return httpd_resp_send_err(req, HTTPD_429_TOO_MANY_REQUESTS, "rate limit");
    char *body = read_body_alloc(req, 2048); if (!body) return httpd_resp_send_500(req);
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "scenes");
    scenes_config_t sc = {};
    if (cJSON_IsArray(arr)) {
        int i = 0; cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (i >= MAX_SCENES) break;
            scene_t *s = &sc.items[i];
            jstr_apply(it, "name", s->name, sizeof(s->name));
            bool en = true; jbool_apply(it, "enabled", &en); s->enabled = en;
            cJSON *acts = cJSON_GetObjectItemCaseSensitive(it, "actions");
            if (cJSON_IsArray(acts)) {
                int a = 0; cJSON *ait;
                cJSON_ArrayForEach(ait, acts) {
                    if (a >= MAX_SCENE_ACTIONS) break;
                    scene_action_t *ac = &s->actions[a];
                    jstr_apply(ait, "code", ac->code, sizeof(ac->code));
                    jstr_apply(ait, "target", ac->target, sizeof(ac->target));
                    jstr_apply(ait, "param", ac->param, sizeof(ac->param));
                    jstr_apply(ait, "value", ac->value, sizeof(ac->value));
                    a++;
                }
                s->action_count = a;
            }
            i++;
        }
        sc.count = i;
    }
    cJSON_Delete(j);
    config_manager_save_scenes(&sc);
    event_log_add(EV_INFO, "http", "Sceny zapisane (kreator): %d", sc.count);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* --- Krok kreatora: Harmonogramy --- */
static esp_err_t schedules_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    schedules_config_t sc = {}; config_manager_load_schedules(&sc);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < sc.count && i < MAX_SCHEDULES; i++) {
        schedule_t *s = &sc.items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s->name);
        cJSON_AddBoolToObject(o, "enabled", s->enabled);
        cJSON_AddNumberToObject(o, "days_mask", s->days_mask);
        cJSON_AddNumberToObject(o, "hour", s->hour);
        cJSON_AddNumberToObject(o, "minute", s->minute);
        cJSON_AddStringToObject(o, "scene_name", s->scene_name);
        cJSON_AddBoolToObject(o, "set_mode", s->set_mode);
        cJSON_AddNumberToObject(o, "target_mode", s->target_mode);
        cJSON_AddItemToArray(arr, o);
    }
    char *out = cJSON_PrintUnformatted(arr); cJSON_Delete(arr);
    return json_send(req, out);
}
static esp_err_t schedules_post(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    if (!rate_limit_allow("api_sched_set", 10, 600)) return httpd_resp_send_err(req, HTTPD_429_TOO_MANY_REQUESTS, "rate limit");
    char *body = read_body_alloc(req, 2048); if (!body) return httpd_resp_send_500(req);
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "schedules");
    schedules_config_t sc = {};
    if (cJSON_IsArray(arr)) {
        int i = 0; cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (i >= MAX_SCHEDULES) break;
            schedule_t *s = &sc.items[i];
            jstr_apply(it, "name", s->name, sizeof(s->name));
            bool en = true; jbool_apply(it, "enabled", &en); s->enabled = en;
            int dm = 0; jint_apply(it, "days_mask", &dm); s->days_mask = (uint8_t)dm;
            int hh = 0; jint_apply(it, "hour", &hh); s->hour = (uint8_t)hh;
            int mm = 0; jint_apply(it, "minute", &mm); s->minute = (uint8_t)mm;
            jstr_apply(it, "scene_name", s->scene_name, sizeof(s->scene_name));
            bool sm = false; jbool_apply(it, "set_mode", &sm); s->set_mode = sm;
            int tm = 0; jint_apply(it, "target_mode", &tm); s->target_mode = (uint8_t)tm;
            i++;
        }
        sc.count = i;
    }
    cJSON_Delete(j);
    config_manager_save_schedules(&sc);
    event_log_add(EV_INFO, "http", "Harmonogramy zapisane (kreator): %d", sc.count);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* --- Krok kreatora: Eskalacja --- */
static esp_err_t escalation_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    escalation_config_t ec = {}; config_manager_load_escalation(&ec);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", ec.enabled);
    cJSON_AddNumberToObject(o, "ack_timeout_sec", ec.ack_timeout_sec);
    cJSON *levels = cJSON_CreateArray();
    for (int i = 0; i < ec.level_count && i < MAX_ESCALATION_LEVELS; i++) {
        escalation_level_t *lv = &ec.levels[i];
        cJSON *lo = cJSON_CreateObject();
        cJSON_AddNumberToObject(lo, "after_sec", lv->after_sec);
        cJSON_AddBoolToObject(lo, "via_webhook", lv->via_webhook);
        cJSON_AddBoolToObject(lo, "via_mqtt", lv->via_mqtt);
        cJSON_AddStringToObject(lo, "note", lv->note);
        cJSON_AddItemToArray(levels, lo);
    }
    cJSON_AddItemToObject(o, "levels", levels);
    char *out = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    return json_send(req, out);
}
static esp_err_t escalation_post(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    if (!rate_limit_allow("api_escal_set", 10, 600)) return httpd_resp_send_err(req, HTTPD_429_TOO_MANY_REQUESTS, "rate limit");
    char *body = read_body_alloc(req, 1024); if (!body) return httpd_resp_send_500(req);
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    escalation_config_t ec = {};
    bool en = false; jbool_apply(j, "enabled", &en); ec.enabled = en;
    int ack = 300; jint_apply(j, "ack_timeout_sec", &ack); ec.ack_timeout_sec = (uint16_t)ack;
    cJSON *levels = cJSON_GetObjectItemCaseSensitive(j, "levels");
    if (cJSON_IsArray(levels)) {
        int i = 0; cJSON *it;
        cJSON_ArrayForEach(it, levels) {
            if (i >= MAX_ESCALATION_LEVELS) break;
            escalation_level_t *lv = &ec.levels[i];
            int as = 0; jint_apply(it, "after_sec", &as); lv->after_sec = (uint16_t)as;
            bool vw = false; jbool_apply(it, "via_webhook", &vw); lv->via_webhook = vw;
            bool vm = false; jbool_apply(it, "via_mqtt", &vm); lv->via_mqtt = vm;
            jstr_apply(it, "note", lv->note, sizeof(lv->note));
            i++;
        }
        ec.level_count = i;
    }
    cJSON_Delete(j);
    config_manager_save_escalation(&ec);
    event_log_add(EV_INFO, "http", "Eskalacja zapisana (kreator), poziomow: %d", ec.level_count);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* --- Postep kreatora (do wznowienia konfiguracji po przerwie) --- */
static esp_err_t wizard_state_get(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    wizard_progress_t wp = {}; config_manager_load_wizard_progress(&wp);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "step", wp.current_step);
    cJSON_AddNumberToObject(o, "completed", wp.completed_mask);
    cJSON_AddBoolToObject(o, "finished", wp.finished);
    char *out = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    return json_send(req, out);
}
static esp_err_t wizard_advance_post(httpd_req_t *req) {
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    char body[128] = {}; if (read_body(req, body, sizeof(body)) != ESP_OK) return httpd_resp_send_500(req);
    cJSON *j = cJSON_Parse(body); if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    wizard_progress_t wp = {}; config_manager_load_wizard_progress(&wp);
    int step = wp.current_step; jint_apply(j, "step", &step);
    bool fin = wp.finished; jbool_apply(j, "finished", &fin);
    cJSON_Delete(j);
    if (step >= 0 && step < 16) wp.completed_mask |= (uint16_t)(1u << step);
    wp.current_step = (uint8_t)(step + 1);
    wp.finished = fin;
    config_manager_save_wizard_progress(&wp);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t http_server_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); cfg.server_port = 80; cfg.max_uri_handlers = 40; if (httpd_start(&s_server, &cfg) != ESP_OK) return ESP_FAIL; s_status.running = true;
    httpd_uri_t uris[] = {
        {.uri="/", .method=HTTP_GET, .handler=root_get}, {.uri="/api/status", .method=HTTP_GET, .handler=status_get}, {.uri="/api/system", .method=HTTP_GET, .handler=system_get}, {.uri="/api/tls", .method=HTTP_GET, .handler=tls_get}, {.uri="/api/ota", .method=HTTP_GET, .handler=ota_get}, {.uri="/api/ota", .method=HTTP_POST, .handler=ota_post}, {.uri="/api/mode", .method=HTTP_GET, .handler=mode_get}, {.uri="/api/mode", .method=HTTP_POST, .handler=mode_post}, {.uri="/api/rate-limit", .method=HTTP_GET, .handler=rate_get}, {.uri="/api/watchdog", .method=HTTP_GET, .handler=watchdog_get}, {.uri="/api/events", .method=HTTP_GET, .handler=events_get}, {.uri="/api/mqtt", .method=HTTP_GET, .handler=mqtt_get}, {.uri="/api/webhook", .method=HTTP_GET, .handler=webhook_get},
        {.uri="/api/mqtt", .method=HTTP_POST, .handler=mqtt_set}, {.uri="/api/auth", .method=HTTP_POST, .handler=auth_post},
        {.uri="/api/net-config", .method=HTTP_GET, .handler=net_config_get}, {.uri="/api/net-config", .method=HTTP_POST, .handler=net_config_post},
        {.uri="/api/tuya-config", .method=HTTP_GET, .handler=tuya_config_get}, {.uri="/api/tuya-config", .method=HTTP_POST, .handler=tuya_config_post},
        {.uri="/api/satel-config", .method=HTTP_GET, .handler=satel_config_get}, {.uri="/api/satel-config", .method=HTTP_POST, .handler=satel_config_post},
        {.uri="/api/webhook-config", .method=HTTP_GET, .handler=webhook_config_get}, {.uri="/api/webhook-config", .method=HTTP_POST, .handler=webhook_config_post},
        {.uri="/api/scenes", .method=HTTP_GET, .handler=scenes_get}, {.uri="/api/scenes", .method=HTTP_POST, .handler=scenes_post},
        {.uri="/api/schedules", .method=HTTP_GET, .handler=schedules_get}, {.uri="/api/schedules", .method=HTTP_POST, .handler=schedules_post},
        {.uri="/api/escalation", .method=HTTP_GET, .handler=escalation_get}, {.uri="/api/escalation", .method=HTTP_POST, .handler=escalation_post},
        {.uri="/api/wizard-state", .method=HTTP_GET, .handler=wizard_state_get}, {.uri="/api/wizard-advance", .method=HTTP_POST, .handler=wizard_advance_post},
    };
    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);
    ESP_LOGI(TAG, "HTTP server started on :80 with Basic Auth + salted SHA-256");
    return ESP_OK;
}
esp_err_t http_server_stop(void) { if (s_server) httpd_stop(s_server); s_server = NULL; s_status.running = false; return ESP_OK; }
void http_server_push_event(const char *event, const char *data) { ESP_LOGI(TAG, "event: %s %s", event ? event : "", data ? data : ""); }
const http_server_status_t *http_server_get_status(void) { return &s_status; }
char *http_server_status_json(void) { char *buf = (char*)malloc(160); if (!buf) return NULL; snprintf(buf, 160, "{\"running\":%s,\"auth_enabled\":%s,\"username\":\"%s\"}", s_status.running?"true":"false", s_status.auth_enabled?"true":"false", s_status.username); return buf; }
