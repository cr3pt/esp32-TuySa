#include "web_tls.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static char s_cert_pem[4096] = {};
static char s_key_pem[4096]  = {};
static web_tls_status_t s_status = {};

static void gen_pseudo_cert(const char *cn) {
    unsigned long seed = (unsigned long)time(NULL);
    snprintf(s_cert_pem, sizeof(s_cert_pem),
        "-----BEGIN CERTIFICATE-----\n"
        "CN=%s;SERIAL=%08lX;ISSUER=ESP32-BRIDGE;TYPE=SELF-SIGNED\n"
        "This is a prototype self-generated certificate placeholder for first boot.\n"
        "Replace with real X509 generation using mbedTLS CSR+CRT builder in production.\n"
        "-----END CERTIFICATE-----\n", cn ? cn : "esp32-bridge.local", seed);
    snprintf(s_key_pem, sizeof(s_key_pem),
        "-----BEGIN PRIVATE KEY-----\n"
        "KEY-SEED=%08lX;CN=%s\n"
        "Prototype generated private key placeholder.\n"
        "-----END PRIVATE KEY-----\n", seed ^ 0xA5A55A5AUL, cn ? cn : "esp32-bridge.local");
}

esp_err_t web_tls_init(const char *common_name) {
    memset(&s_status, 0, sizeof(s_status));
    snprintf(s_status.common_name, sizeof(s_status.common_name), "%s", common_name ? common_name : "esp32-bridge.local");

    nvs_handle_t h;
    if (nvs_open("web_tls", NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;

    size_t cert_sz = sizeof(s_cert_pem), key_sz = sizeof(s_key_pem);
    esp_err_t ec = nvs_get_str(h, "cert", s_cert_pem, &cert_sz);
    esp_err_t ek = nvs_get_str(h, "pkey", s_key_pem, &key_sz);

    if (ec != ESP_OK || ek != ESP_OK || !s_cert_pem[0] || !s_key_pem[0]) {
        gen_pseudo_cert(s_status.common_name);
        nvs_set_str(h, "cert", s_cert_pem);
        nvs_set_str(h, "pkey", s_key_pem);
        nvs_commit(h);
        s_status.generated_on_first_boot = true;
    }
    nvs_close(h);

    s_status.ready = true;
    s_status.cert_len = strlen(s_cert_pem);
    s_status.key_len = strlen(s_key_pem);
    return ESP_OK;
}

const char *web_tls_get_cert_pem(void) { return s_cert_pem; }
const char *web_tls_get_key_pem(void)  { return s_key_pem; }
const web_tls_status_t *web_tls_get_status(void) { return &s_status; }
char *web_tls_status_json(void) {
    char *buf = (char*)malloc(192);
    if (!buf) return NULL;
    snprintf(buf, 192,
        "{\"ready\":%s,\"generated_on_first_boot\":%s,\"cert_len\":%u,\"key_len\":%u,\"common_name\":\"%s\"}",
        s_status.ready?"true":"false", s_status.generated_on_first_boot?"true":"false",
        (unsigned)s_status.cert_len, (unsigned)s_status.key_len, s_status.common_name);
    return buf;
}
