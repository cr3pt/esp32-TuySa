/**
 * Runtime HTTP Server — ESP32 TUYA+SATEL Bridge
 * ─────────────────────────────────────────────
 * Obsługuje:
 *   GET  /                      → index.html z SPIFFS (gzip)
 *   GET  /static/*              → pliki z SPIFFS (css, js, gzip)
 *   GET  /api/status            → JSON: WiFi, uptime, wersja, stan systemów
 *   GET  /api/config/net        → JSON: konfiguracja sieci (bez haseł)
 *   POST /api/config/net        → zapis konfiguracji sieci
 *   GET  /api/config/tuya       → JSON: dane TUYA (secret zamaskowany)
 *   POST /api/config/tuya       → zapis danych TUYA
 *   GET  /api/config/satel      → JSON: dane SATEL (password zamaskowany)
 *   POST /api/config/satel      → zapis danych SATEL
 *   POST /api/test              → test połączenia {"target":"tuya"|"satel"}
 *   POST /api/restart           → kontrolowany restart
 *   POST /api/reset             → factory reset
 *   GET  /api/events            → SSE stream (Server-Sent Events)
 */
#include "http_server.h"
#include "spiffs_www.h"
#include "config_manager.h"
#include "wifi_manager.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "HTTPD";

/* ── Konfiguracja ────────────────────────────────────────────────────────── */
#define HTTP_PORT          80
#define MAX_SSE_CLIENTS    4
#define BODY_BUF_SIZE      2048
#define AUTH_BUF_SIZE      128

static httpd_handle_t s_server    = NULL;
static http_test_cb_t  s_test_cb  = NULL;
static http_reset_cb_t s_reset_cb = NULL;
static char s_auth_user[64]       = {};
static char s_auth_pass[64]       = {};
static bool s_auth_enabled        = false;

/* SSE — lista aktywnych połączeń */
static httpd_handle_t s_sse_hdl[MAX_SSE_CLIENTS] = {};
static int            s_sse_fd[MAX_SSE_CLIENTS]   = {-1,-1,-1,-1};
static SemaphoreHandle_t s_sse_mutex              = NULL;

/* ── Basic Auth ──────────────────────────────────────────────────────────── */
static bool check_auth(httpd_req_t *req) {
    if (!s_auth_enabled) return true;

    char auth_hdr[AUTH_BUF_SIZE] = {};
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                    auth_hdr, sizeof(auth_hdr)) != ESP_OK)
        goto deny;

    if (strncmp(auth_hdr, "Basic ", 6) != 0) goto deny;

    {
        unsigned char decoded[AUTH_BUF_SIZE] = {};
        size_t out_len = 0;
        if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1,
                                  &out_len,
                                  (const unsigned char *)auth_hdr + 6,
                                  strlen(auth_hdr + 6)) != 0)
            goto deny;
        decoded[out_len] = '\0';

        /* Format: "user:pass" */
        char expected[AUTH_BUF_SIZE];
        snprintf(expected, sizeof(expected), "%s:%s", s_auth_user, s_auth_pass);
        if (strcmp((char *)decoded, expected) == 0) return true;
    }

deny:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"ESP32 Bridge\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
}

/* ── Pomocnicze: wczytaj body ────────────────────────────────────────────── */
static int read_body(httpd_req_t *req, char *buf, size_t maxlen) {
    int total = 0, r;
    while ((r = httpd_req_recv(req, buf + total,
                               maxlen - total - 1)) > 0)
        total += r;
    buf[total] = '\0';
    return total;
}

/* ── Pomocnicze: JSON tiny parser (klucz → wartość) ─────────────────────── */
static bool json_get_str(const char *json, const char *key,
                          char *out, size_t outlen) {
    /* Szuka "key":"value" lub "key":value */
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    bool quoted = (*p == '"');
    if (quoted) p++;
    const char *end = quoted ? strchr(p, '"') : strpbrk(p, ",}");
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= outlen) len = outlen - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool def) {
    char tmp[8] = {};
    if (!json_get_str(json, key, tmp, sizeof(tmp))) return def;
    return (strcmp(tmp, "true") == 0 || strcmp(tmp, "1") == 0);
}

/* ── Pomocnicze: ustaw nagłówki CORS + no-cache ──────────────────────────── */
static void set_json_headers(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

/* ═══════════════════════════════════════════════════════════════════════════
   HANDLERY
   ═══════════════════════════════════════════════════════════════════════════ */

/* ── GET / → index.html z SPIFFS ─────────────────────────────────────────── */
static esp_err_t handle_root(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    return spiffs_www_serve(req, "/www/index.html");
}

/* ── GET /static/* ───────────────────────────────────────────────────────── */
static esp_err_t handle_static(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    char path[128];
    snprintf(path, sizeof(path), "/www%s", req->uri);
    return spiffs_www_serve(req, path);
}

/* ── GET /api/status ─────────────────────────────────────────────────────── */
static esp_err_t handle_status(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    set_json_headers(req);

    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    wifi_state_t ws   = wifi_manager_get_state();
    uint32_t ip       = wifi_manager_get_ip();

    /* Adres IP jako string */
    char ip_str[16] = "0.0.0.0";
    if (ip) {
        snprintf(ip_str, sizeof(ip_str), "%lu.%lu.%lu.%lu",
                 (ip) & 0xFF, (ip >> 8) & 0xFF,
                 (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{"
        "\"version\":\"0.3.0\","
        "\"uptime_s\":%lu,"
        "\"wifi_state\":%d,"
        "\"ip\":\"%s\","
        "\"tuya_ok\":false,"    /* etap 5 nadpisze to */
        "\"satel_ok\":false,"   /* etap 6 nadpisze to */
        "\"free_heap\":%lu"
        "}",
        (unsigned long)uptime_s,
        (int)ws,
        ip_str,
        (unsigned long)esp_get_free_heap_size());

    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── GET /api/config/net ─────────────────────────────────────────────────── */
static esp_err_t handle_get_net(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    set_json_headers(req);

    net_config_t net = {};
    config_manager_load_net(&net);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{"
        "\"ssid\":\"%s\","
        "\"hostname\":\"%s\","
        "\"ntp\":\"%s\","
        "\"dns1\":\"%s\","
        "\"dns2\":\"%s\","
        "\"dhcp\":%s,"
        "\"static_ip\":\"%s\","
        "\"static_gw\":\"%s\","
        "\"static_nm\":\"%s\""
        "}",
        net.ssid, net.hostname, net.ntp_server,
        net.dns1, net.dns2,
        net.dhcp ? "true" : "false",
        net.static_ip, net.static_gw, net.static_nm);

    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── POST /api/config/net ────────────────────────────────────────────────── */
static esp_err_t handle_post_net(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char body[BODY_BUF_SIZE] = {};
    read_body(req, body, sizeof(body));

    net_config_t net = {};
    if (!json_get_str(body, "ssid",     net.ssid,       sizeof(net.ssid)) ||
        strlen(net.ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"error\":\"Brak SSID\"}");
        return ESP_OK;
    }
    json_get_str(body, "password",  net.password,    sizeof(net.password));
    json_get_str(body, "hostname",  net.hostname,    sizeof(net.hostname));
    json_get_str(body, "ntp",       net.ntp_server,  sizeof(net.ntp_server));
    json_get_str(body, "dns1",      net.dns1,        sizeof(net.dns1));
    json_get_str(body, "dns2",      net.dns2,        sizeof(net.dns2));
    net.dhcp = json_get_bool(body, "dhcp", true);
    if (!net.dhcp) {
        json_get_str(body, "static_ip", net.static_ip, sizeof(net.static_ip));
        json_get_str(body, "static_gw", net.static_gw, sizeof(net.static_gw));
        json_get_str(body, "static_nm", net.static_nm, sizeof(net.static_nm));
    }

    esp_err_t err = config_manager_save_net(&net);
    set_json_headers(req);
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
        http_server_push_event("config_changed", "{\"section\":\"net\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "{\"error\":\"Błąd zapisu NVS\"}");
    }
    return ESP_OK;
}

/* ── GET /api/config/tuya ────────────────────────────────────────────────── */
static esp_err_t handle_get_tuya(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    set_json_headers(req);

    tuya_creds_t c = {};
    config_manager_load_tuya(&c);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"region\":\"%s\",\"client_id\":\"%s\","
        "\"client_secret\":\"***\",\"user_uid\":\"%s\"}",
        c.region, c.client_id, c.user_uid);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── POST /api/config/tuya ───────────────────────────────────────────────── */
static esp_err_t handle_post_tuya(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char body[BODY_BUF_SIZE] = {};
    read_body(req, body, sizeof(body));

    tuya_creds_t c = {};
    json_get_str(body, "region",        c.region,        sizeof(c.region));
    json_get_str(body, "client_id",     c.client_id,     sizeof(c.client_id));
    json_get_str(body, "client_secret", c.client_secret, sizeof(c.client_secret));
    json_get_str(body, "user_uid",      c.user_uid,      sizeof(c.user_uid));

    if (strlen(c.client_id) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "{\"error\":\"Brak client_id\"}");
        return ESP_OK;
    }

    set_json_headers(req);
    esp_err_t err = config_manager_save_tuya(&c);
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
        http_server_push_event("config_changed", "{\"section\":\"tuya\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "{\"error\":\"Błąd zapisu\"}");
    }
    return ESP_OK;
}

/* ── GET /api/config/satel ───────────────────────────────────────────────── */
static esp_err_t handle_get_satel(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    set_json_headers(req);

    satel_creds_t c = {};
    config_manager_load_satel(&c);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"host\":\"%s\",\"port\":%u,"
        "\"password\":\"***\",\"panel_id\":\"%s\"}",
        c.host, c.port, c.panel_id);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── POST /api/config/satel ──────────────────────────────────────────────── */
static esp_err_t handle_post_satel(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char body[BODY_BUF_SIZE] = {};
    read_body(req, body, sizeof(body));

    satel_creds_t c = {};
    json_get_str(body, "host",     c.host,     sizeof(c.host));
    char port_s[8] = {};
    json_get_str(body, "port",     port_s,     sizeof(port_s));
    c.port = (uint16_t)atoi(port_s);
    json_get_str(body, "password", c.password, sizeof(c.password));
    json_get_str(body, "panel_id", c.panel_id, sizeof(c.panel_id));

    if (strlen(c.host) == 0 || c.port == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "{\"error\":\"Brak host lub port\"}");
        return ESP_OK;
    }

    set_json_headers(req);
    esp_err_t err = config_manager_save_satel(&c);
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
        http_server_push_event("config_changed", "{\"section\":\"satel\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "{\"error\":\"Błąd zapisu\"}");
    }
    return ESP_OK;
}

/* ── POST /api/test ──────────────────────────────────────────────────────── */
static esp_err_t handle_test(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char body[128] = {};
    read_body(req, body, sizeof(body));
    char target[16] = {};
    json_get_str(body, "target", target, sizeof(target));

    set_json_headers(req);
    if (s_test_cb) {
        esp_err_t err = s_test_cb(target);
        if (err == ESP_OK) {
            char resp[64];
            snprintf(resp, sizeof(resp), "{\"ok\":true,\"target\":\"%s\"}", target);
            httpd_resp_sendstr(req, resp);
        } else {
            char resp[96];
            snprintf(resp, sizeof(resp),
                     "{\"ok\":false,\"target\":\"%s\",\"err\":\"0x%x\"}",
                     target, err);
            httpd_resp_sendstr(req, resp);
        }
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"test_cb not registered\"}");
    }
    return ESP_OK;
}

/* ── POST /api/restart ───────────────────────────────────────────────────── */
static esp_err_t handle_restart(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    set_json_headers(req);
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Restart za 2s\"}");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

/* ── POST /api/reset ─────────────────────────────────────────────────────── */
static esp_err_t handle_reset(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    set_json_headers(req);
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Factory reset za 2s\"}");
    vTaskDelay(pdMS_TO_TICKS(2000));
    config_manager_erase_all();
    if (s_reset_cb) s_reset_cb();
    esp_restart();
    return ESP_OK;
}

/* ── GET /api/events — Server-Sent Events ────────────────────────────────── */
static esp_err_t handle_sse(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    /* Nagłówki SSE */
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection",    "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    /* Pierwsza wiadomość: potwierdzenie połączenia */
    httpd_resp_sendstr_chunk(req, "data: {\"connected\":true}\n\n");

    /* Zarejestruj klienta SSE */
    int fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_sse_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (s_sse_fd[i] < 0) {
            s_sse_fd[i]  = fd;
            s_sse_hdl[i] = req->handle;
            break;
        }
    }
    xSemaphoreGive(s_sse_mutex);

    ESP_LOGI(TAG, "SSE klient połączony fd=%d", fd);

    /* Trzymaj połączenie otwarte – klient czeka na push */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        /* Heartbeat co 15 s, żeby proxy/firewall nie zamknął połączenia */
        if (httpd_resp_sendstr_chunk(req, ": heartbeat\n\n") != ESP_OK) break;
    }

    /* Wyrejestruj klienta */
    xSemaphoreTake(s_sse_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (s_sse_fd[i] == fd) { s_sse_fd[i] = -1; s_sse_hdl[i] = NULL; }
    }
    xSemaphoreGive(s_sse_mutex);

    httpd_resp_sendstr_chunk(req, NULL);  /* zakończ transfer */
    return ESP_OK;
}

/* ── Push SSE do wszystkich klientów ─────────────────────────────────────── */
void http_server_push_event(const char *event_name, const char *json_data) {
    if (!s_sse_mutex) return;
    char msg[256];
    snprintf(msg, sizeof(msg), "event: %s\ndata: %s\n\n", event_name, json_data);

    xSemaphoreTake(s_sse_mutex, pdMS_TO_TICKS(100));
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (s_sse_fd[i] >= 0 && s_sse_hdl[i]) {
            /* Wyślij bezpośrednio przez socket – handler SSE jest zablokowany */
            send(s_sse_fd[i], msg, strlen(msg), MSG_DONTWAIT);
        }
    }
    xSemaphoreGive(s_sse_mutex);
}

/* ── Start / Stop ────────────────────────────────────────────────────────── */
esp_err_t http_server_start(const char *username, const char *password) {
    if (s_server) return ESP_OK;

    s_sse_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) s_sse_fd[i] = -1;

    if (username && password) {
        strncpy(s_auth_user, username, sizeof(s_auth_user) - 1);
        strncpy(s_auth_pass, password, sizeof(s_auth_pass) - 1);
        s_auth_enabled = true;
        ESP_LOGI(TAG, "Basic Auth włączone (user='%s')", s_auth_user);
    }

    httpd_config_t cfg       = HTTPD_DEFAULT_CONFIG();
    cfg.server_port          = HTTP_PORT;
    cfg.max_uri_handlers     = 20;
    cfg.stack_size           = 10240;
    cfg.uri_match_fn         = httpd_uri_match_wildcard;

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));

    /* Rejestracja handlerów */
    #define REG(u, m, h) do { \
        httpd_uri_t _u = {(u),(m),(h),NULL}; \
        httpd_register_uri_handler(s_server, &_u); \
    } while(0)

    REG("/",                  HTTP_GET,  handle_root);
    REG("/static/*",          HTTP_GET,  handle_static);
    REG("/api/status",        HTTP_GET,  handle_status);
    REG("/api/config/net",    HTTP_GET,  handle_get_net);
    REG("/api/config/net",    HTTP_POST, handle_post_net);
    REG("/api/config/tuya",   HTTP_GET,  handle_get_tuya);
    REG("/api/config/tuya",   HTTP_POST, handle_post_tuya);
    REG("/api/config/satel",  HTTP_GET,  handle_get_satel);
    REG("/api/config/satel",  HTTP_POST, handle_post_satel);
    REG("/api/test",          HTTP_POST, handle_test);
    REG("/api/restart",       HTTP_POST, handle_restart);
    REG("/api/reset",         HTTP_POST, handle_reset);
    REG("/api/events",        HTTP_GET,  handle_sse);
    #undef REG

    ESP_LOGI(TAG, "Runtime HTTP server uruchomiony na porcie %d", HTTP_PORT);
    return ESP_OK;
}

esp_err_t http_server_stop(void) {
    if (!s_server) return ESP_OK;
    httpd_stop(s_server);
    s_server = NULL;
    return ESP_OK;
}

void http_server_set_test_cb(http_test_cb_t cb)   { s_test_cb  = cb; }
void http_server_set_reset_cb(http_reset_cb_t cb) { s_reset_cb = cb; }
