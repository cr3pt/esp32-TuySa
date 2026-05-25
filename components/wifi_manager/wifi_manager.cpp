#include "wifi_manager.h"
#include "config_manager.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      5

static EventGroupHandle_t  s_wifi_event_group = NULL;
static wifi_state_t        s_state            = WIFI_STATE_IDLE;
static uint32_t            s_ip               = 0;
static wifi_state_cb_t     s_cb               = NULL;
static int                 s_retry_num        = 0;
static bool                s_initialized      = false;
static esp_netif_t        *s_netif_sta        = NULL;
static esp_netif_t        *s_netif_ap         = NULL;

/* ── Pomocnicze ─────────────────────────────────────────────────────────── */
static void set_state(wifi_state_t st, uint32_t ip) {
    s_state = st;
    s_ip    = ip;
    if (s_cb) s_cb(st, ip);
}

/* ── Event handler ──────────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            set_state(WIFI_STATE_CONNECTING, 0);
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_retry_num < WIFI_MAX_RETRY) {
                s_retry_num++;
                ESP_LOGW(TAG, "Rozłączono – próba %d/%d", s_retry_num, WIFI_MAX_RETRY);
                set_state(WIFI_STATE_CONNECTING, 0);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "Nie udało się połączyć z WiFi po %d próbach", WIFI_MAX_RETRY);
                set_state(WIFI_STATE_FAILED, 0);
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "Klient AP połączony, AID=%d", e->aid);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_retry_num = 0;
        ESP_LOGI(TAG, "Uzyskano IP: " IPSTR, IP2STR(&e->ip_info.ip));
        set_state(WIFI_STATE_CONNECTED, e->ip_info.ip.addr);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── API publiczne ──────────────────────────────────────────────────────── */
esp_err_t wifi_manager_init(void) {
    if (s_initialized) return ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi Manager zainicjalizowany");
    return ESP_OK;
}

esp_err_t wifi_manager_start_ap(const char *ssid) {
    s_netif_ap = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {};
    strncpy((char *)ap_cfg.ap.ssid, ssid ? ssid : "ESP32-Bridge-Setup",
            sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len       = 0;          /* auto */
    ap_cfg.ap.channel        = 1;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 3;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    set_state(WIFI_STATE_AP_ACTIVE, 0);
    ESP_LOGI(TAG, "AP aktywny – SSID: '%s'  (brak hasła)", ap_cfg.ap.ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_start_sta(uint32_t timeout_ms) {
    s_netif_sta = esp_netif_create_default_wifi_sta();

    /* Załaduj konfigurację z NVS */
    net_config_t net = {};
    esp_err_t err = config_manager_load_net(&net);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Brak konfiguracji sieci w NVS");
        return err;
    }

    /* Statyczne IP jeśli wymagane */
    if (!net.dhcp) {
        esp_netif_ip_info_t ip_info = {};
        ip4addr_aton(net.static_ip, (ip4_addr_t *)&ip_info.ip);
        ip4addr_aton(net.static_gw, (ip4_addr_t *)&ip_info.gw);
        ip4addr_aton(net.static_nm, (ip4_addr_t *)&ip_info.netmask);
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(s_netif_sta));
        ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif_sta, &ip_info));
        ESP_LOGI(TAG, "Statyczne IP: %s", net.static_ip);
    }

    /* Hostname */
    esp_netif_set_hostname(s_netif_sta, net.hostname);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta_cfg = {};
    strncpy((char *)sta_cfg.sta.ssid,     net.ssid,     sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, net.password, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Czekaj na wynik */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi połączone z '%s'", net.ssid);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Nie połączono z WiFi (timeout=%lu ms)", (unsigned long)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

void wifi_manager_set_state_cb(wifi_state_cb_t cb) { s_cb = cb; }
wifi_state_t wifi_manager_get_state(void)          { return s_state; }
uint32_t     wifi_manager_get_ip(void)             { return s_ip; }

esp_err_t wifi_manager_stop(void) {
    esp_wifi_stop();
    esp_wifi_deinit();
    s_initialized = false;
    set_state(WIFI_STATE_IDLE, 0);
    return ESP_OK;
}
