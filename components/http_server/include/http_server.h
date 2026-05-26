#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef esp_err_t(*test_cb_t)(const char *target);
typedef esp_err_t(*set_key_cb_t)(const char *pass);
typedef void(*reset_cb_t)(void);
typedef char*(*status_cb_t)(void);
typedef char*(*rules_list_cb_t)(void);
typedef esp_err_t(*rules_add_cb_t)(const char *json);
typedef esp_err_t(*rules_del_cb_t)(const char *id);
typedef esp_err_t(*rules_fire_cb_t)(const char *id);
esp_err_t http_server_start(const char *user, const char *pass);
void http_server_push_event(const char *event, const char *data);
void http_server_set_test_cb(test_cb_t cb);
void http_server_set_key_cb(set_key_cb_t cb);
void http_server_set_reset_cb(reset_cb_t cb);
void http_server_set_status_cb(status_cb_t cb);
void http_server_set_rules_api_cb(rules_list_cb_t list, rules_add_cb_t add, rules_del_cb_t del, rules_fire_cb_t fire);
#ifdef __cplusplus
}
#endif
