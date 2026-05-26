#pragma once
#include "esp_err.h"
#include "config_manager.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t crypto_load_satel_creds(satel_creds_t *c, size_t sz);
esp_err_t crypto_save_satel_creds(const satel_creds_t *c, size_t sz);
esp_err_t crypto_load_tuya_creds(tuya_creds_t *c, size_t sz);
esp_err_t crypto_save_tuya_creds(const tuya_creds_t *c, size_t sz);
esp_err_t crypto_manager_set_key_from_passphrase(const char *pass, size_t len);
void crypto_erase_secrets(void);
bool crypto_manager_is_ready(void);
#ifdef __cplusplus
}
#endif
