#include "satel_client.h"
#include "crypto_manager.h"
#include "http_server.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

static const char *TAG = "SATEL";

#define SATEL_POLL_INTERVAL_MS   2000
#define SATEL_CONNECT_TIMEOUT_MS 5000
#define SATEL_RECV_TIMEOUT_MS    3000
#define SATEL_RECONNECT_DELAY_MS 5000
#define SATEL_RECV_BUF_SIZE      512

static char           s_host[64]     = {};
static uint16_t       s_port         = 7094;
static char           s_pass[16]     = {};
static char           s_ethm_ip[64]  = {}; /* IP modulu dla wejsc IP */

static satel_state_t       s_state      = SATEL_STATE_IDLE;
static satel_panel_state_t s_panel      = {};
static satel_panel_state_t s_prev       = {};
static int                 s_sock       = -1;
static int                 s_reconnects = 0;
static TaskHandle_t        s_task       = NULL;
static SemaphoreHandle_t   s_mutex      = NULL;

static satel_zone_cb_t          s_zone_cb    = NULL;
static void                    *s_zone_user  = NULL;
static satel_output_cb_t        s_out_cb     = NULL;
static void                    *s_out_user   = NULL;
static satel_state_change_cb_t  s_state_cb   = NULL;
static void                    *s_state_user = NULL;

/* Komendy do wyslania w kolejce */
static uint8_t s_cmd_queue[8][SATEL_MAX_FRAME_LEN];
static size_t  s_cmd_len[8];
static int     s_cmd_head = 0, s_cmd_tail = 0;

static void set_state(satel_state_t st) {
    s_state = st;
    if (s_state_cb) s_state_cb(st, s_state_user);
    const char *names[] = {"IDLE","CONNECTING","READY","POLLING","ERROR","RECONNECT"};
    ESP_LOGI(TAG, "Stan: %s", names[st < 6 ? st : 0]);
    char ev[48]; snprintf(ev, sizeof(ev), "{\"state\":\"%s\"}", names[st < 6 ? st : 0]);
    http_server_push_event("satel_state", ev);
}

static int tcp_connect(void) {
    struct addrinfo hints = {}, *res = NULL;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%d", s_port);
    if (getaddrinfo(s_host, port_str, &hints, &res) != 0 || !res) return -1;
    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = {SATEL_CONNECT_TIMEOUT_MS/1000, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        close(sock); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    struct timeval tv2 = {SATEL_RECV_TIMEOUT_MS/1000, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
    return sock;
}

static esp_err_t send_frame(int sock, uint8_t cmd, const uint8_t *data, size_t dlen) {
    uint8_t frame[SATEL_MAX_FRAME_LEN];
    size_t  flen = satel_build_frame(cmd, data, dlen, s_pass, frame, sizeof(frame));
    if (!flen) return ESP_ERR_INVALID_SIZE;
    int sent = send(sock, frame, flen, 0);
    return (sent == (int)flen) ? ESP_OK : ESP_FAIL;
}

static satel_parse_result_t recv_frame(int sock, satel_frame_t *frame) {
    static uint8_t rbuf[SATEL_RECV_BUF_SIZE];
    static size_t  rbuf_len = 0;

    /* Odbior danych do bufora */
    int n = recv(sock, rbuf + rbuf_len, sizeof(rbuf) - rbuf_len - 1, 0);
    if (n <= 0) return SATEL_PARSE_ERROR;
    rbuf_len += n;

    size_t consumed = 0;
    satel_parse_result_t r = satel_parse_buffer(rbuf, rbuf_len, frame, &consumed);
    if (consumed > 0 && consumed <= rbuf_len) {
        memmove(rbuf, rbuf + consumed, rbuf_len - consumed);
        rbuf_len -= consumed;
    }
    return r;
}

static void notify_changes(void) {
    if (!s_zone_cb && !s_out_cb) return;
    /* Porownaj strefy */
    if (s_zone_cb) {
        for (int z = 1; z <= SATEL_MAX_ZONES; z++) {
            bool now  = satel_bit_get(s_panel.zones_violated, z);
            bool prev = satel_bit_get(s_prev.zones_violated,  z);
            if (now != prev) s_zone_cb(z, now, s_zone_user);
        }
    }
    /* Porownaj wyjscia */
    if (s_out_cb) {
        for (int o = 1; o <= SATEL_MAX_OUTPUTS; o++) {
            bool now  = satel_bit_get(s_panel.outputs_state, o);
            bool prev = satel_bit_get(s_prev.outputs_state,  o);
            if (now != prev) s_out_cb(o, now, s_out_user);
        }
    }
    s_prev = s_panel;
}

/* Sekwencja pollingu — zapytaj o wszystkie stany */
static const uint8_t POLL_CMDS[] = {
    SATEL_CMD_ZONES_VIOLATION,
    SATEL_CMD_ZONES_TAMPER,
    SATEL_CMD_ZONES_ALARM,
    SATEL_CMD_ZONES_BYPASSED,
    SATEL_CMD_OUTPUTS_STATE,
    SATEL_CMD_PARTITIONS_ARMED,
    SATEL_CMD_PARTITIONS_ALARM,
};

static esp_err_t poll_once(int sock) {
    for (size_t i = 0; i < sizeof(POLL_CMDS); i++) {
        if (send_frame(sock, POLL_CMDS[i], NULL, 0) != ESP_OK) return ESP_FAIL;
        satel_frame_t frame = {};
        satel_parse_result_t r = recv_frame(sock, &frame);
        if (r == SATEL_PARSE_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            satel_apply_frame(&frame, &s_panel);
            xSemaphoreGive(s_mutex);
        } else if (r == SATEL_PARSE_ERROR) {
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    notify_changes();
    return ESP_OK;
}

static void enqueue_cmd(uint8_t cmd, const uint8_t *data, size_t dlen) {
    int next = (s_cmd_tail + 1) % 8;
    if (next == s_cmd_head) return; /* kolejka pelna */
    size_t flen = satel_build_frame(cmd, data, dlen, s_pass,
                                    s_cmd_queue[s_cmd_tail], SATEL_MAX_FRAME_LEN);
    s_cmd_len[s_cmd_tail] = flen;
    s_cmd_tail = next;
}

static void flush_cmd_queue(int sock) {
    while (s_cmd_head != s_cmd_tail) {
        if (s_cmd_len[s_cmd_head] > 0)
            send(sock, s_cmd_queue[s_cmd_head], s_cmd_len[s_cmd_head], 0);
        s_cmd_head = (s_cmd_head + 1) % 8;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void satel_task(void *) {
    while (true) {
        if (!s_host[0]) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }

        set_state(SATEL_STATE_CONNECTING);
        s_sock = tcp_connect();

        if (s_sock < 0) {
            ESP_LOGE(TAG, "Blad polaczenia TCP z %s:%d", s_host, s_port);
            set_state(SATEL_STATE_ERROR);
            s_reconnects++;
            vTaskDelay(pdMS_TO_TICKS(SATEL_RECONNECT_DELAY_MS));
            set_state(SATEL_STATE_RECONNECT);
            continue;
        }

        ESP_LOGI(TAG, "Polaczono z %s:%d", s_host, s_port);
        set_state(SATEL_STATE_READY);

        /* Natychmiastowy pelny polling po polaczeniu */
        poll_once(s_sock);

        while (true) {
            set_state(SATEL_STATE_POLLING);
            flush_cmd_queue(s_sock);

            if (poll_once(s_sock) != ESP_OK) {
                ESP_LOGW(TAG, "Utrata polaczenia SATEL, reconnect za %ds",
                         SATEL_RECONNECT_DELAY_MS/1000);
                break;
            }
            set_state(SATEL_STATE_READY);
            vTaskDelay(pdMS_TO_TICKS(SATEL_POLL_INTERVAL_MS));
        }

        close(s_sock); s_sock = -1;
        set_state(SATEL_STATE_ERROR);
        s_reconnects++;
        vTaskDelay(pdMS_TO_TICKS(SATEL_RECONNECT_DELAY_MS));
        set_state(SATEL_STATE_RECONNECT);
    }
}

/* --- API publiczne --- */
esp_err_t satel_client_init(void) {
    satel_creds_t c = {};
    if (crypto_load_satel_creds(&c, sizeof(c)) != ESP_OK) return ESP_ERR_NOT_FOUND;
    strncpy(s_host, c.host, sizeof(s_host)-1);
    s_port = c.port ? c.port : 7094;
    strncpy(s_pass, c.password, sizeof(s_pass)-1);
    strncpy(s_ethm_ip, c.host, sizeof(s_ethm_ip)-1); /* ten sam IP dla wejsc IP */
    s_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Init: %s:%d", s_host, s_port);
    return ESP_OK;
}

esp_err_t satel_client_start(void) {
    return xTaskCreate(satel_task, "satel_client", 6144, NULL, 6, &s_task)
           == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t satel_client_stop(void) {
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    return ESP_OK;
}

esp_err_t satel_client_test(void) {
    if (!s_host[0]) return ESP_ERR_INVALID_STATE;
    int sock = tcp_connect();
    if (sock < 0) return ESP_FAIL;
    close(sock);
    return ESP_OK;
}

satel_state_t satel_client_get_state(void)            { return s_state; }
int           satel_client_get_reconnect_count(void)  { return s_reconnects; }
const satel_panel_state_t *satel_client_get_panel_state(void) { return &s_panel; }

bool satel_is_zone_violated(uint8_t z)  { return satel_bit_get(s_panel.zones_violated, z); }
bool satel_is_zone_alarmed(uint8_t z)   { return satel_bit_get(s_panel.zones_alarm, z); }
bool satel_is_output_active(uint8_t o)  { return satel_bit_get(s_panel.outputs_state, o); }
bool satel_is_partition_armed(uint8_t p){ return satel_bit_get(s_panel.partitions_armed, p); }
int  satel_get_violated_count(void)     { return satel_bit_count(s_panel.zones_violated, 16); }

esp_err_t satel_output_on(uint8_t out) {
    uint8_t d[4] = {}; d[(out-1)/8] |= (1 << ((out-1)%8));
    enqueue_cmd(SATEL_CMD_OUTPUT_ON, d, 4); return ESP_OK;
}
esp_err_t satel_output_off(uint8_t out) {
    uint8_t d[4] = {}; d[(out-1)/8] |= (1 << ((out-1)%8));
    enqueue_cmd(SATEL_CMD_OUTPUT_OFF, d, 4); return ESP_OK;
}
esp_err_t satel_arm(uint8_t partition, uint8_t mode) {
    uint8_t d[4] = {}; d[(partition-1)/8] |= (1 << ((partition-1)%8));
    uint8_t cmd = SATEL_CMD_ARM_MODE0 + (mode & 0x03);
    enqueue_cmd(cmd, d, 4); return ESP_OK;
}
esp_err_t satel_disarm(uint8_t partition) {
    uint8_t d[4] = {}; d[(partition-1)/8] |= (1 << ((partition-1)%8));
    enqueue_cmd(SATEL_CMD_DISARM, d, 4); return ESP_OK;
}
esp_err_t satel_clear_alarm(uint8_t partition) {
    uint8_t d[4] = {}; d[(partition-1)/8] |= (1 << ((partition-1)%8));
    enqueue_cmd(SATEL_CMD_CLEAR_ALARM, d, 4); return ESP_OK;
}

/* Wejscia IP: HTTP GET do ETHM-1 PLUS np. http://192.168.1.100/satel/input?n=1&v=1 */
esp_err_t satel_ip_input_on(uint8_t n) {
    char url[128]; snprintf(url, sizeof(url), "http://%s/satel/input?n=%d&v=1", s_ethm_ip, n);
    esp_http_client_config_t cfg = {}; cfg.url = url;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_err_t e = esp_http_client_perform(h);
    esp_http_client_cleanup(h);
    ESP_LOGI(TAG, "IP INPUT ON %d -> %s", n, e==ESP_OK?"OK":"FAIL");
    return e;
}
esp_err_t satel_ip_input_off(uint8_t n) {
    char url[128]; snprintf(url, sizeof(url), "http://%s/satel/input?n=%d&v=0", s_ethm_ip, n);
    esp_http_client_config_t cfg = {}; cfg.url = url;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_err_t e = esp_http_client_perform(h);
    esp_http_client_cleanup(h);
    ESP_LOGI(TAG, "IP INPUT OFF %d -> %s", n, e==ESP_OK?"OK":"FAIL");
    return e;
}

void satel_set_zone_cb(satel_zone_cb_t cb, void *u)           { s_zone_cb=cb; s_zone_user=u; }
void satel_set_output_cb(satel_output_cb_t cb, void *u)        { s_out_cb=cb;  s_out_user=u;  }
void satel_set_state_cb(satel_state_change_cb_t cb, void *u)   { s_state_cb=cb; s_state_user=u; }

char *satel_state_to_json(void) {
    char *b = (char*)malloc(512);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int vz = satel_bit_count(s_panel.zones_violated,   16);
    int ap = satel_bit_count(s_panel.partitions_armed,  4);
    int al = satel_bit_count(s_panel.partitions_alarm,  4);
    int oz = satel_bit_count(s_panel.zones_alarm,       16);
    xSemaphoreGive(s_mutex);
    const char *st[] = {"idle","connecting","ready","polling","error","reconnect"};
    snprintf(b, 512,
        "{\"connected\":%s,\"state\":\"%s\",\"panel\":\"SATEL INTEGRA\","
        "\"violated_zones\":%d,\"alarm_zones\":%d,\"armed_parts\":%d,"
        "\"alarm_parts\":%d,\"reconnects\":%d}",
        (s_state==SATEL_STATE_READY||s_state==SATEL_STATE_POLLING)?"true":"false",
        st[s_state<6?s_state:0], vz, oz, ap, al, s_reconnects);
    return b;
}
