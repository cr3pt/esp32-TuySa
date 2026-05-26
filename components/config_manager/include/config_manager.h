#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { char ssid[64]; char password[64]; char hostname[32]; bool dhcp; char ip[16]; char gw[16]; char mask[16]; char dns1[16]; char dns2[16]; char ntp_server[64]; } net_config_t;
typedef struct { char region[8]; char client_id[48]; char client_secret[48]; char user_uid[48]; } tuya_creds_t;
typedef struct { char host[64]; uint16_t port; char password[16]; char panel_id[32]; } satel_creds_t;
esp_err_t config_manager_save_net(const net_config_t *cfg);
esp_err_t config_manager_load_net(net_config_t *cfg);
esp_err_t config_manager_erase_all(void);
typedef struct { char username[32]; char password[64]; } panel_auth_t;
esp_err_t config_manager_save_panel_auth(const panel_auth_t *cfg);
esp_err_t config_manager_load_panel_auth(panel_auth_t *cfg);

#ifdef __cplusplus
}
#endif
