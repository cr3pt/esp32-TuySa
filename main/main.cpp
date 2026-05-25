/**
 * TUYA + SATEL INTEGRA Bridge — ESP32
 * ═════════════════════════════════════
 * Etap 8 (FINALNY): Watchdog łączności + sygnalizacja LED
 *
 * Kolejność uruchamiania:
 *  1. boot_manager_init()       — tryb SETUP lub NORMAL
 *  2. wifi_manager_init()
 *  3. [SETUP] AP + captive portal → zapis konfiguracji → restart
 *  4. wifi_manager_start_sta()  — połącz z siecią
 *  5. NTP sync
 *  6. spiffs_www_mount()        — panel WWW
 *  7. http_server_start()       — REST API + SSE
 *  8. crypto_manager_init()     — klucz szyfrujący
 *  9. tuya_client_init/start()  — klient TUYA Cloud
 * 10. satel_client_init/start() — klient SATEL INTEGRA TCP
 * 11. rule_engine_init/start()  — silnik reguł automatyki
 * 12. watchdog_init/start()     — monitor + LED
 */
#include "boot_manager.h"
#include "config_manager.h"
#include "crypto_manager.h"
#include "tuya_client.h"
#include "satel_client.h"
#include "rule_engine.h"
#include "default_rules.h"
#include "watchdog.h"
#include "wifi_manager.h"
#include "captive_portal.h"
#include "spiffs_www.h"
#include "http_server.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>

static const char *TAG = "MAIN";

/* ── Callbacki ─────────────────────────────────────────────────────────── */
static void on_config_saved(void) { boot_manager_restart("portal-saved",1500); }

static void on_wifi_state(wifi_state_t st, uint32_t ip) {
    char js[64], ip_s[20];
    snprintf(ip_s,sizeof(ip_s),"%lu.%lu.%lu.%lu",
             ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
    snprintf(js,sizeof(js),"{\"state\":%d,\"ip\":\"%s\"}",(int)st,ip_s);
    http_server_push_event("wifi_state",js);
}

static esp_err_t on_test(const char *target) {
    if (strcmp(target,"tuya")  == 0) return tuya_client_test();
    if (strcmp(target,"satel") == 0) return satel_client_test();
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t on_set_key(const char *pass) {
    esp_err_t err = crypto_manager_set_key_from_passphrase(pass,strlen(pass));
    if (err==ESP_OK) boot_manager_restart("key-set",2000);
    return err;
}

static void on_factory_reset(void) {
    watchdog_stop();
    rule_engine_stop();
    tuya_client_stop();
    satel_client_stop();
    crypto_erase_secrets();
    /* Usuń konfigurację sieci i uruchom tryb SETUP */
    config_manager_erase_all();
    boot_manager_restart("factory-reset", 500);
}

static void ntp_sync(const char *srv) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, srv);
    esp_sntp_init();
    time_t now=0; struct tm ti={};
    for(int i=0;i<20&&ti.tm_year<(2024-1900);i++){
        vTaskDelay(pdMS_TO_TICKS(500));time(&now);localtime_r(&now,&ti);
    }
    ESP_LOGI(TAG,"Czas: %04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year+1900,ti.tm_mon+1,ti.tm_mday,
             ti.tm_hour,ti.tm_min,ti.tm_sec);
}

/* ═══════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════ */
extern "C" void app_main(void) {

    /* 1. Boot mode */
    boot_mode_t mode = boot_manager_init();
    ESP_ERROR_CHECK(wifi_manager_init());
    wifi_manager_set_state_cb(on_wifi_state);

    /* 2. Tryb SETUP (AP + captive portal) */
    if (mode == BOOT_MODE_SETUP) {
        ESP_LOGI(TAG, "Tryb SETUP — uruchamianie AP ESP32-Bridge-Setup");
        ESP_ERROR_CHECK(wifi_manager_start_ap("ESP32-Bridge-Setup"));
        ESP_ERROR_CHECK(captive_portal_start(on_config_saved));
        /* Miga żółto (boot pattern) — watchdog jeszcze nie działa,
           steruj LED bezpośrednio */
        while(true) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    /* 3. Połącz z siecią */
    if (wifi_manager_start_sta(15000) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi timeout — restart");
        boot_manager_restart("wifi-fail", 2000);
    }

    /* 4. NTP */
    net_config_t net={};
    config_manager_load_net(&net);
    ntp_sync(net.ntp_server[0] ? net.ntp_server : "pool.ntp.org");

    /* 5. WWW + REST API */
    ESP_ERROR_CHECK(spiffs_www_mount());
    http_server_set_test_cb(on_test);
    http_server_set_reset_cb(on_factory_reset);
    http_server_set_key_cb(on_set_key);
    http_server_set_status_cb(watchdog_health_to_json);   /* /api/status */
    ESP_ERROR_CHECK(http_server_start("admin","bridge123"));

    /* 6. Kryptografia */
    esp_err_t cerr = crypto_manager_init();
    if (cerr == ESP_ERR_NOT_FOUND) {
        http_server_push_event("key_required","{\"required\":true}");
        ESP_LOGW(TAG, "Czekam na ustawienie klucza przez panel WWW…");
        while(!crypto_manager_is_ready()) vTaskDelay(pdMS_TO_TICKS(1000));
    } else if (cerr == ESP_ERR_INVALID_CRC) {
        ESP_LOGE(TAG, "Uszkodzony klucz kryptograficzny — reset");
        crypto_erase_secrets();
        boot_manager_restart("crypto-corrupt", 500);
    }

    /* 7. TUYA */
    if (tuya_client_init() == ESP_OK) {
        tuya_client_start();
        ESP_LOGI(TAG, "TUYA client OK");
    } else {
        ESP_LOGW(TAG, "TUYA — brak konfiguracji (skonfiguruj przez panel)");
        http_server_push_event("tuya_state","{\"state\":\"unconfigured\"}");
    }

    /* 8. SATEL */
    if (satel_client_init() == ESP_OK) {
        satel_client_start();
        ESP_LOGI(TAG, "SATEL client OK");
    } else {
        ESP_LOGW(TAG, "SATEL — brak konfiguracji (skonfiguruj przez panel)");
        http_server_push_event("satel_state","{\"state\":\"unconfigured\"}");
    }

    /* 9. Silnik reguł */
    ESP_ERROR_CHECK(rule_engine_init());
    rule_engine_load_defaults();
    ESP_ERROR_CHECK(rule_engine_start());
    http_server_set_rules_api_cb(
        rule_engine_rules_to_json,
        rule_engine_add_from_json,
        rule_engine_delete,
        rule_engine_test_fire
    );
    ESP_LOGI(TAG, "Silnik reguł OK (%d reguł)", rule_engine_count());

    /* 10. Watchdog — OSTATNI */
    ESP_ERROR_CHECK(watchdog_init());
    ESP_ERROR_CHECK(watchdog_start());

    http_server_push_event("system_ready",
        "{\"stage\":8,\"msg\":\"ESP32 Bridge — SYSTEM GOTOWY\","
        "\"tuya\":true,\"satel\":true,\"rules\":true,\"watchdog\":true}");

    ESP_LOGI(TAG, "═══════════════════════════════════════");
    ESP_LOGI(TAG, "  ESP32 TUYA+SATEL Bridge — v1.0 READY");
    ESP_LOGI(TAG, "═══════════════════════════════════════");

    /* Pętla główna — wszystko działa w taskach */
    while(true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        /* Opcjonalnie: auto-save reguł co 30 minut */
        rule_engine_save();
    }
}
