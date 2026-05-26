#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ntp_synced;
    bool mdns_started;
    char hostname[32];
    char ntp_server[64];
    uint32_t sync_epoch;
} system_services_status_t;

esp_err_t system_services_init(const char *hostname, const char *ntp_server);
esp_err_t system_services_start(void);
esp_err_t system_services_stop(void);
bool      system_services_ntp_synced(void);
uint32_t  system_services_get_epoch(void);
const system_services_status_t *system_services_get_status(void);
char     *system_services_status_json(void);

#ifdef __cplusplus
}
#endif
