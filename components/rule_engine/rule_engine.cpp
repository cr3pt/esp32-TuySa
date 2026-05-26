#include "rule_engine.h"
#include "http_server.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
typedef struct { char id[16]; char name[48]; int enabled; int fire_count; int dry_run; } rule_t;
static rule_t s_rules[8]={{"r1","Czujka salon -> swiatlo",1,0,0},{"r2","Swiatlo -> wejscie IP",1,0,1}}; static int s_count=2;
esp_err_t rule_engine_init(void){ return ESP_OK; }
esp_err_t rule_engine_start(void){ return ESP_OK; }
void rule_engine_stop(void){}
int rule_engine_count(void){ return s_count; }
char *rule_engine_rules_to_json(void){ char *b=(char*)malloc(1024); size_t p=0; p+=snprintf(b+p,1024-p,"{\"rules\":["); for(int i=0;i<s_count;i++){ if(i) p+=snprintf(b+p,1024-p,","); p+=snprintf(b+p,1024-p,"{\"id\":\"%s\",\"name\":\"%s\",\"enabled\":%s,\"dry_run\":%s,\"fire_count\":%d}",s_rules[i].id,s_rules[i].name,s_rules[i].enabled?"true":"false",s_rules[i].dry_run?"true":"false",s_rules[i].fire_count);} snprintf(b+p,1024-p,"]}"); return b; }
esp_err_t rule_engine_add_from_json(const char *json){ (void)json; return ESP_OK; }
esp_err_t rule_engine_delete(const char *id){ for(int i=0;i<s_count;i++){ if(strcmp(s_rules[i].id,id)==0){ for(int j=i;j<s_count-1;j++) s_rules[j]=s_rules[j+1]; s_count--; return ESP_OK; } } return ESP_FAIL; }
esp_err_t rule_engine_test_fire(const char *id){ for(int i=0;i<s_count;i++){ if(strcmp(s_rules[i].id,id)==0){ s_rules[i].fire_count++; http_server_push_event("rule_fired","{\"id\":\"demo\"}"); return ESP_OK; } } return ESP_FAIL; }
void rule_engine_save(void){}
