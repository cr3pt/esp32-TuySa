#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Uruchom uproszczony serwer DNS UDP na porcie 53.
 *        Odpowiada na KAŻDE zapytanie A podanym adresem IPv4.
 * @param redirect_ip IPv4 w notacji sieciowej (np. z esp_netif_get_ip_info).
 */
esp_err_t dns_server_start(uint32_t redirect_ip);
esp_err_t dns_server_stop(void);

#ifdef __cplusplus
}
#endif
