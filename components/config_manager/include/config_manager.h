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
typedef struct { char username[32]; uint8_t salt[16]; uint8_t hash[32]; } panel_auth_t;
typedef struct { char broker[128]; char base_topic[64]; } mqtt_config_t;
typedef struct { char base_url[160]; } webhook_config_t;

/* --- Sceny: grupa akcji TUYA/SATEL wywolywanych razem --- */
#define MAX_SCENES         8
#define MAX_SCENE_ACTIONS  4
typedef struct {
    char code[24];      /* "tuya_bool" | "tuya_int" | "satel_output_on" | "satel_output_off" | "satel_arm" | "satel_disarm" */
    char target[48];    /* tuya dev_id lub numer wyjscia/partycji SATEL jako tekst */
    char param[24];     /* dp code dla TUYA, tryb uzbrojenia (0-3) dla SATEL arm */
    char value[24];     /* wartosc dp dla TUYA (true/false/liczba) */
} scene_action_t;
typedef struct {
    char name[32];
    bool enabled;
    uint8_t action_count;
    scene_action_t actions[MAX_SCENE_ACTIONS];
} scene_t;
typedef struct { uint8_t count; scene_t items[MAX_SCENES]; } scenes_config_t;

/* --- Harmonogramy: wyzwalanie sceny/trybu o okreslonej porze --- */
#define MAX_SCHEDULES 8
typedef struct {
    char name[32];
    bool enabled;
    uint8_t days_mask;    /* bit0=Pn ... bit6=Nd */
    uint8_t hour;
    uint8_t minute;
    char scene_name[32];  /* pusty = brak sceny do wywolania */
    bool set_mode;
    uint8_t target_mode;  /* bridge_mode_t: 0=HOME 1=AWAY 2=NIGHT 3=MANUAL, uzyte tylko gdy set_mode */
} schedule_t;
typedef struct { uint8_t count; schedule_t items[MAX_SCHEDULES]; } schedules_config_t;

/* --- Eskalacja: kolejne poziomy powiadomien, gdy alarm nie zostanie potwierdzony --- */
#define MAX_ESCALATION_LEVELS 4
typedef struct {
    uint16_t after_sec;   /* opoznienie od wyzwolenia / poprzedniego poziomu */
    bool via_webhook;
    bool via_mqtt;
    char note[48];        /* opis odbiorcy/kanalu, np. "SMS: +48600000000" */
} escalation_level_t;
typedef struct {
    bool enabled;
    uint16_t ack_timeout_sec; /* czas na potwierdzenie zanim eskalacja ruszy */
    uint8_t level_count;
    escalation_level_t levels[MAX_ESCALATION_LEVELS];
} escalation_config_t;

/* --- Postep kreatora, do wznowienia konfiguracji po przerwie --- */
typedef struct { uint8_t current_step; uint16_t completed_mask; bool finished; } wizard_progress_t;

esp_err_t config_manager_save_net(const net_config_t *cfg);
esp_err_t config_manager_load_net(net_config_t *cfg);
esp_err_t config_manager_save_panel_auth(const panel_auth_t *cfg);
esp_err_t config_manager_load_panel_auth(panel_auth_t *cfg);
esp_err_t config_manager_save_mqtt(const mqtt_config_t *cfg);
esp_err_t config_manager_load_mqtt(mqtt_config_t *cfg);
esp_err_t config_manager_save_webhook(const webhook_config_t *cfg);
esp_err_t config_manager_load_webhook(webhook_config_t *cfg);
esp_err_t config_manager_save_scenes(const scenes_config_t *cfg);
esp_err_t config_manager_load_scenes(scenes_config_t *cfg);
esp_err_t config_manager_save_schedules(const schedules_config_t *cfg);
esp_err_t config_manager_load_schedules(schedules_config_t *cfg);
esp_err_t config_manager_save_escalation(const escalation_config_t *cfg);
esp_err_t config_manager_load_escalation(escalation_config_t *cfg);
esp_err_t config_manager_save_wizard_progress(const wizard_progress_t *cfg);
esp_err_t config_manager_load_wizard_progress(wizard_progress_t *cfg);
esp_err_t config_manager_erase_all(void);
#ifdef __cplusplus
}
#endif
