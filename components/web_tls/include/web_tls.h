#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ready;
    bool generated_on_first_boot;
    size_t cert_len;
    size_t key_len;
    char common_name[64];
} web_tls_status_t;

esp_err_t web_tls_init(const char *common_name);
const char *web_tls_get_cert_pem(void);
const char *web_tls_get_key_pem(void);
const web_tls_status_t *web_tls_get_status(void);
char *web_tls_status_json(void);

#ifdef __cplusplus
}
#endif
