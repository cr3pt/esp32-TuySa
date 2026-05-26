#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t mdns_wrapper_init(const char *hostname);
esp_err_t mdns_wrapper_set_hostname(const char *hostname);
#ifdef __cplusplus
}
#endif
