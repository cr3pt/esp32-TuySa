/**
 * Captive Portal — serwer HTTP z formularzem konfiguracyjnym.
 *
 * Obsługiwane endpointy:
 *   GET  /              → formularz HTML (wszystkie parametry konfiguracyjne)
 *   POST /save          → walidacja + zapis przez config_manager + restart
 *   GET  /status        → JSON ze stanem (dla AJAX polling w formularzu)
 *   GET  /hotspot-detect.html   → przekierowanie (iOS)
 *   GET  /generate_204          → 302 (Android)
 *   GET  /connecttest.txt       → przekierowanie (Windows)
 */
#include "captive_portal.h"
#include "dns_server.h"
#include "config_manager.h"
#include "wifi_manager.h"

#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG        = "PORTAL";
static httpd_handle_t s_httpd = NULL;
static portal_saved_cb_t s_saved_cb = NULL;

/* ── Strona HTML (inlined, ok. 4 KB skompresowane do ~2 KB gzip w etapie 3) */
static const char SETUP_HTML[] =
"<!DOCTYPE html>"
"<html lang='pl'>"
"<head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 Bridge — Konfiguracja</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,sans-serif;background:#111;color:#e0e0e0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:1rem}"
".card{background:#1c1b19;border:1px solid #333;border-radius:1rem;padding:2rem;width:100%;max-width:540px;box-shadow:0 12px 40px rgba(0,0,0,.5)}"
"h1{font-size:1.4rem;margin-bottom:.25rem}"
".sub{color:#888;font-size:.875rem;margin-bottom:1.5rem}"
"fieldset{border:1px solid #333;border-radius:.5rem;padding:1rem;margin-bottom:1.25rem}"
"legend{font-size:.75rem;text-transform:uppercase;letter-spacing:.08em;color:#888;padding:0 .4rem}"
".row{display:grid;grid-template-columns:1fr 1fr;gap:.75rem}"
".field{display:grid;gap:.35rem;margin-bottom:.75rem}"
"label{font-size:.8rem;font-weight:700}"
"input,select{min-height:42px;padding:.65rem .9rem;background:#111;border:1px solid #444;border-radius:.4rem;color:#e0e0e0;font-size:.9rem;width:100%}"
"input:focus,select:focus{outline:2px solid #4f98a3;outline-offset:2px}"
".hint{font-size:.75rem;color:#666}"
".btn{min-height:46px;padding:.9rem 1.5rem;background:#4f98a3;color:#111;border:none;border-radius:.75rem;font-size:1rem;font-weight:700;cursor:pointer;width:100%;transition:.18s}"
".btn:hover{background:#227f8b}"
"#msg{margin-top:1rem;padding:.75rem;border-radius:.5rem;display:none;font-size:.875rem}"
".ok{background:#1e3f0a;border:1px solid #437a22;color:#6daa45}"
".er{background:#3a0a1e;border:1px solid #a12c7b;color:#d163a7}"
"#static-fields{display:none}"
"</style>"
"</head>"
"<body>"
"<div class='card'>"
"<h1>&#x1F4E1; ESP32 Bridge Setup</h1>"
"<p class='sub'>Pierwsza konfiguracja — TUYA + SATEL INTEGRA / ETHM-1 PLUS</p>"
"<form id='frm' method='POST' action='/save'>"

"<fieldset><legend>Sie&#263; WiFi</legend>"
"<div class='row'>"
"<div class='field'><label>SSID</label><input name='ssid' required maxlength='63' placeholder='Nazwa sieci'/></div>"
"<div class='field'><label>Has&#322;o WiFi</label><input name='wifi_pass' type='password' maxlength='63'/></div>"
"</div>"
"<div class='row'>"
"<div class='field'><label>Nazwa hosta</label><input name='hostname' value='esp32-bridge' maxlength='63'/></div>"
"<div class='field'><label>Tryb adresacji</label>"
"<select name='dhcp' onchange=\"document.getElementById('static-fields').style.display=this.value==='0'?'block':'none'\">"
"<option value='1'>DHCP (automatyczny)</option>"
"<option value='0'>Statyczny IP</option>"
"</select></div>"
"</div>"
"<div id='static-fields'>"
"<div class='row'>"
"<div class='field'><label>Adres IP</label><input name='static_ip' placeholder='192.168.1.50'/></div>"
"<div class='field'><label>Brama</label><input name='static_gw' placeholder='192.168.1.1'/></div>"
"</div>"
"<div class='row'>"
"<div class='field'><label>Maska podsieci</label><input name='static_nm' placeholder='255.255.255.0'/></div>"
"</div>"
"</div>"
"<div class='row'>"
"<div class='field'><label>DNS 1</label><input name='dns1' value='1.1.1.1'/></div>"
"<div class='field'><label>DNS 2</label><input name='dns2' value='8.8.8.8'/></div>"
"</div>"
"<div class='field'><label>Serwer NTP</label><input name='ntp' value='pool.ntp.org'/></div>"
"</fieldset>"

"<fieldset><legend>Bezpiecze&#324;stwo</legend>"
"<div class='field'>"
"<label>Klucz szyfruj&#261;cy urz&#261;dzenia (min. 12 znak&#243;w)</label>"
"<input name='enc_key' type='password' required minlength='12' maxlength='31' placeholder='Ustaw raz, nie mo&#380;na zmieni&#263; bez factory reset'/>"
"<div class='hint'>Tym kluczem zostan&#261; zaszyfrowane dane dost&#281;powe TUYA i SATEL po pozytywnym te&#347;cie po&#322;&#261;czenia.</div>"
"</div>"
"</fieldset>"

"<button class='btn' type='submit'>Zapisz i uruchom ponownie &#x1F680;</button>"
"<div id='msg'></div>"
"</form>"
"</div>"
"<script>"
"document.getElementById('frm').onsubmit=async function(e){"
"e.preventDefault();"
"const fd=new FormData(this);"
"const msg=document.getElementById('msg');"
"msg.style.display='block';msg.className='ok';msg.textContent='Zapisuję konfigurację…';"
"try{"
"const r=await fetch('/save',{method:'POST',body:new URLSearchParams(fd)});"
"const t=await r.text();"
"if(r.ok){msg.className='ok';msg.textContent='✓ Zapisano! Urządzenie restartuje się…';}"
"else{msg.className='er';msg.textContent='✗ Błąd: '+t;}"
"}catch(ex){msg.className='er';msg.textContent='✗ Błąd połączenia: '+ex;}"
"};"
"</script>"
"</body></html>";

/* ── Pomocnicze: odczyt URL-encoded body ─────────────────────────────────── */
static char *get_form_value(const char *body, const char *key) {
    /* Szuka key=value& w URL-encoded body */
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *end = strchr(p, '&');
            size_t vlen = end ? (size_t)(end - p) : strlen(p);
            char *val = (char *)malloc(vlen + 1);
            if (!val) return NULL;
            memcpy(val, p, vlen);
            val[vlen] = '\0';
            /* URL-decode: zamień '+' na spację */
            for (size_t i = 0; i < vlen; i++) if (val[i] == '+') val[i] = ' ';
            return val;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return NULL;
}

/* ── Handlery HTTP ───────────────────────────────────────────────────────── */
static esp_err_t handler_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handler_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_save(httpd_req_t *req) {
    /* Wczytaj body */
    char body[1024] = {};
    int total = 0, len;
    while ((len = httpd_req_recv(req, body + total,
                                 sizeof(body) - total - 1)) > 0) {
        total += len;
    }
    body[total] = '\0';
    ESP_LOGI(TAG, "POST /save body (%d B)", total);

    /* Rozparsuj pola */
    #define GV(k) get_form_value(body, k)
    char *ssid      = GV("ssid");
    char *wpass     = GV("wifi_pass");
    char *hostname  = GV("hostname");
    char *dhcp_s    = GV("dhcp");
    char *sip       = GV("static_ip");
    char *sgw       = GV("static_gw");
    char *snm       = GV("static_nm");
    char *dns1      = GV("dns1");
    char *dns2      = GV("dns2");
    char *ntp       = GV("ntp");
    char *enc_key_s = GV("enc_key");
    #undef GV

    if (!ssid || strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Brak SSID");
        goto cleanup;
    }
    if (!enc_key_s || strlen(enc_key_s) < 12) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Klucz szyfrujący musi mieć min. 12 znaków");
        goto cleanup;
    }

    {
        /* Wypełnij struktury i zapisz */
        net_config_t net = {};
        strncpy(net.ssid,       ssid,     sizeof(net.ssid) - 1);
        strncpy(net.password,   wpass    ? wpass    : "", sizeof(net.password) - 1);
        strncpy(net.hostname,   hostname ? hostname : "esp32-bridge", sizeof(net.hostname) - 1);
        strncpy(net.ntp_server, ntp      ? ntp      : "pool.ntp.org", sizeof(net.ntp_server) - 1);
        strncpy(net.dns1,       dns1     ? dns1     : "1.1.1.1", sizeof(net.dns1) - 1);
        strncpy(net.dns2,       dns2     ? dns2     : "8.8.8.8", sizeof(net.dns2) - 1);
        net.dhcp = (dhcp_s && dhcp_s[0] == '1');
        if (!net.dhcp) {
            strncpy(net.static_ip, sip ? sip : "", sizeof(net.static_ip) - 1);
            strncpy(net.static_gw, sgw ? sgw : "", sizeof(net.static_gw) - 1);
            strncpy(net.static_nm, snm ? snm : "", sizeof(net.static_nm) - 1);
        }

        /* Klucz szyfrujący — wypełnij 32 bajty (pad zerami) */
        uint8_t enc_key[CFG_ENC_KEY_LEN] = {};
        size_t kl = strlen(enc_key_s);
        if (kl > CFG_ENC_KEY_LEN) kl = CFG_ENC_KEY_LEN;
        memcpy(enc_key, enc_key_s, kl);

        esp_err_t err = config_manager_save_net(&net);
        if (err == ESP_OK) err = config_manager_save_enc_key(enc_key);

        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Błąd zapisu konfiguracji");
            goto cleanup;
        }

        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "OK");

        ESP_LOGI(TAG, "Konfiguracja zapisana – planuję restart za 1,5 s");
        if (s_saved_cb) s_saved_cb();
    }

cleanup:
    free(ssid); free(wpass); free(hostname); free(dhcp_s);
    free(sip); free(sgw); free(snm); free(dns1); free(dns2);
    free(ntp); free(enc_key_s);
    return ESP_OK;
}

static esp_err_t handler_status(httpd_req_t *req) {
    wifi_state_t st = wifi_manager_get_state();
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"wifi_state\":%d,\"ip\":%lu}",
             (int)st, (unsigned long)wifi_manager_get_ip());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── Start / Stop ────────────────────────────────────────────────────────── */
esp_err_t captive_portal_start(portal_saved_cb_t cb) {
    s_saved_cb = cb;

    /* Pobierz IP interfejsu AP */
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info = {};
    if (ap_netif) esp_netif_get_ip_info(ap_netif, &ip_info);
    uint32_t ap_ip = ip_info.ip.addr;

    /* Serwer DNS */
    dns_server_start(ap_ip);

    /* Serwer HTTP */
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size        = 8192;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &cfg));

    const httpd_uri_t uris[] = {
        { "/",                    HTTP_GET,  handler_root,     NULL },
        { "/save",                HTTP_POST, handler_save,     NULL },
        { "/status",              HTTP_GET,  handler_status,   NULL },
        /* Captive portal redirects dla różnych systemów operacyjnych */
        { "/hotspot-detect.html", HTTP_GET,  handler_redirect, NULL },
        { "/generate_204",        HTTP_GET,  handler_redirect, NULL },
        { "/connecttest.txt",     HTTP_GET,  handler_redirect, NULL },
        { "/redirect",            HTTP_GET,  handler_redirect, NULL },
        { "/canonical.html",      HTTP_GET,  handler_redirect, NULL },
    };
    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }

    ESP_LOGI(TAG, "Captive Portal uruchomiony — http://192.168.4.1/");
    return ESP_OK;
}

esp_err_t captive_portal_stop(void) {
    dns_server_stop();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    return ESP_OK;
}
