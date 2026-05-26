#include "web_tls.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/x509write_crt.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/sha256.h"
#include "mbedtls/error.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static char s_cert_pem[4096] = {};
static char s_key_pem[4096]  = {};
static web_tls_status_t s_status = {};

static int gen_x509_self_signed(const char *cn, char *cert, size_t cert_len, char *key, size_t key_len) {
    int ret = 0;
    mbedtls_pk_context key_ctx;
    mbedtls_x509write_cert crt;
    mbedtls_mpi serial;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    const char *pers = "esp32_bridge_tls";
    unsigned char cert_buf[4096] = {0};

    mbedtls_pk_init(&key_ctx);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi_init(&serial);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, strlen(pers))) != 0) goto cleanup;
    if ((ret = mbedtls_pk_setup(&key_ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA))) != 0) goto cleanup;
    if ((ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key_ctx), mbedtls_ctr_drbg_random, &ctr_drbg, 2048, 65537)) != 0) goto cleanup;
    if ((ret = mbedtls_mpi_read_string(&serial, 10, "1")) != 0) goto cleanup;

    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key_ctx);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key_ctx);
    mbedtls_x509write_crt_set_serial(&crt, &serial);

    char subject[128];
    snprintf(subject, sizeof(subject), "CN=%s,O=ESP32 Bridge,C=PL", cn ? cn : "esp32-bridge.local");
    if ((ret = mbedtls_x509write_crt_set_subject_name(&crt, subject)) != 0) goto cleanup;
    if ((ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject)) != 0) goto cleanup;
    if ((ret = mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20360101000000")) != 0) goto cleanup;
    if ((ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) != 0) goto cleanup;
    if ((ret = mbedtls_x509write_crt_set_subject_key_identifier(&crt)) != 0) goto cleanup;
    if ((ret = mbedtls_x509write_crt_set_authority_key_identifier(&crt)) != 0) goto cleanup;

    ret = mbedtls_x509write_crt_pem(&crt, cert_buf, sizeof(cert_buf), mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) goto cleanup;
    snprintf(cert, cert_len, "%s", (char*)cert_buf);

    unsigned char key_buf[4096] = {0};
    ret = mbedtls_pk_write_key_pem(&key_ctx, key_buf, sizeof(key_buf));
    if (ret != 0) goto cleanup;
    snprintf(key, key_len, "%s", (char*)key_buf);
    ret = 0;
cleanup:
    mbedtls_pk_free(&key_ctx);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_mpi_free(&serial);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
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
        if (gen_x509_self_signed(s_status.common_name, s_cert_pem, sizeof(s_cert_pem), s_key_pem, sizeof(s_key_pem)) != 0) {
            nvs_close(h); return ESP_FAIL;
        }
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
char *web_tls_status_json(void) { char *buf = (char*)malloc(192); if (!buf) return NULL; snprintf(buf, 192, "{\"ready\":%s,\"generated_on_first_boot\":%s,\"cert_len\":%u,\"key_len\":%u,\"common_name\":\"%s\"}", s_status.ready?"true":"false", s_status.generated_on_first_boot?"true":"false", (unsigned)s_status.cert_len, (unsigned)s_status.key_len, s_status.common_name); return buf; }
