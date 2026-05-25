#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Montuje partycję SPIFFS pod /www.
 *        Musi być wywołana przed http_server_start().
 */
esp_err_t spiffs_www_mount(void);

/**
 * @brief Odmontuj SPIFFS.
 */
esp_err_t spiffs_www_unmount(void);

/**
 * @brief Serwuj plik z SPIFFS do klienta HTTP.
 *        Automatycznie:
 *          - ustawia Content-Type na podstawie rozszerzenia
 *          - wykrywa plik *.gz i dodaje nagłówek Content-Encoding: gzip
 *          - strumieniuje plik w kawałkach 1024 B (oszczędność RAM)
 *
 * @param req    Aktywne żądanie HTTP
 * @param path   Ścieżka na SPIFFS, np. "/www/index.html"
 */
esp_err_t spiffs_www_serve(httpd_req_t *req, const char *path);

#ifdef __cplusplus
}
#endif
