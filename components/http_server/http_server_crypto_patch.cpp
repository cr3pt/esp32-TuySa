/**
 * Patch etap 4 — endpoint /api/config/key
 * Dołącza się do http_server.cpp przez CMakeLists.
 * Obsługa: POST /api/config/key {"passphrase":"..."}
 */
#include "http_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG_KEY = "HTTPD_KEY";
static http_set_key_cb_t s_key_cb = NULL;
void http_server_set_key_cb(http_set_key_cb_t cb) { s_key_cb = cb; }

/* Lokalny json_get_str (duplikat bo to oddzielna jednostka translacji) */
static bool jgs(const char *json, const char *key, char *out, size_t olen) {
    char needle[64]; snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle); if (!p) return false;
    p += strlen(needle); while (*p == ' ') p++;
    bool q = (*p == '"'); if (q) p++;
    const char *e = q ? strchr(p, '"') : strpbrk(p, ",}");
    if (!e) return false;
    size_t l = (size_t)(e-p); if (l >= olen) l = olen-1;
    memcpy(out, p, l); out[l] = '\0';
    return true;
}

static esp_err_t handle_set_key(httpd_req_t *req) {
    char body[256] = {};
    int total = 0, r;
    while ((r = httpd_req_recv(req, body + total,
                               (int)sizeof(body)-total-1)) > 0) total += r;
    body[total] = '\0';

    char pass[64] = {};
    if (!jgs(body, "passphrase", pass, sizeof(pass)) || strlen(pass) < 12) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "{\"error\":\"Hasło min. 12 znaków\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    if (s_key_cb) {
        esp_err_t err = s_key_cb(pass);
        if (err == ESP_OK) {
            httpd_resp_sendstr(req,
                "{\"ok\":true,\"msg\":\"Klucz ustawiony. Urządzenie restartuje się.\"}");
        } else if (err == ESP_ERR_INVALID_STATE) {
            httpd_resp_sendstr(req,
                "{\"ok\":false,\"error\":\"Klucz już ustawiony — factory reset wymagany.\"}");
        } else {
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Błąd ustawiania klucza\"}");
        }
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"key_cb nie zarejestrowany\"}");
    }
    return ESP_OK;
}

/* Funkcja eksportowana — wywoływana z http_server_start() */
extern "C" void http_register_key_handler(httpd_handle_t server) {
    httpd_uri_t u = {"/api/config/key", HTTP_POST, handle_set_key, NULL};
    httpd_register_uri_handler(server, &u);
    ESP_LOGI(TAG_KEY, "Endpoint /api/config/key zarejestrowany");
}
