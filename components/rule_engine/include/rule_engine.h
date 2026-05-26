#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t rule_engine_init(void);
esp_err_t rule_engine_start(void);
void rule_engine_stop(void);
int rule_engine_count(void);
char *rule_engine_rules_to_json(void);
esp_err_t rule_engine_add_from_json(const char *json);
esp_err_t rule_engine_delete(const char *id);
esp_err_t rule_engine_test_fire(const char *id);
void rule_engine_save(void);
#ifdef __cplusplus
}
#endif
