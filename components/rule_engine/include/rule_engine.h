#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Silnik wykonawczy dla scen/harmonogramow/eskalacji skonfigurowanych w kreatorze WWW
   (config_manager: scenes_config_t / schedules_config_t / escalation_config_t).
   Kreator tylko zapisuje te dane - rule_engine odpowiada za ich faktyczne uruchamianie. */

esp_err_t rule_engine_init(void);
esp_err_t rule_engine_start(void);
esp_err_t rule_engine_stop(void);

/* Wykonaj natychmiast nazwana, wlaczona scene (np. z harmonogramu, eskalacji lub recznie z API). */
esp_err_t rule_engine_run_scene(const char *scene_name);

/* Wywolywane przy wykryciu alarmu w SATEL/TUYA - uruchamia zegar eskalacji
   (jesli w kroku "Eskalacja" kreatora jest wlaczona). */
void rule_engine_trigger_escalation(const char *reason);
/* Potwierdzenie alarmu przez operatora - zatrzymuje dalsza eskalacje. */
void rule_engine_ack_escalation(void);
bool rule_engine_escalation_active(void);

char *rule_engine_status_json(void);

#ifdef __cplusplus
}
#endif
