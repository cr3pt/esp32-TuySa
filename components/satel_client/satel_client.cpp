/**
 * satel_client.cpp — Klient SATEL INTEGRA przez ETHM-1 PLUS (TCP)
 * ─────────────────────────────────────────────────────────────────
 * Protokół:
 *   TCP → host:port (domyślnie 7094)
 *   Każde żądanie: ramka binarna SATEL (0xFF 0xFE ... 0xFE 0x0D)
 *   Odpowiedź: ramka binarna z tym samym formatem
 *
 * Task FreeRTOS:
 *   1. Połącz TCP z ETHM-1 PLUS
 *   2. Pobierz typ centrali i wersję firmware
 *   3. Pętla poll:
 *      a) Zapytaj NEW_DATA (0xFF) — czy są zmiany
 *      b) Jeśli tak → odpytaj wszystkie mapy bitowe stanu
 *      c) Porównaj z poprzednim stanem → wywołaj callback dla zmian
 *   4. Przy utracie połączenia: zamknij socket, czekaj, wróć do 1
 *
 * Komendy sterujące (arm/disarm/output/ip_input):
 *   Tworzone przez API i wysyłane przez dedykowaną kolejkę FreeRTOS.
 */

#include "satel_client.h"
#include "satel_protocol.h"
#include "crypto_manager.h"
#include "http_server.h"
#include "config_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

static const char *TAG = "SATEL";

/* ── Konfiguracja ─────────────────────────────────────────────────────────── */
#define POLL_INTERVAL_MS      2000   /* 2s — wystarczające dla centrali alarmowej */
#define HEARTBEAT_MS          30000
#define RECONNECT_DELAY_MS    8000
#define TCP_RECV_TIMEOUT_MS   5000
#define TCP_CONNECT_TIMEOUT_MS 5000
#define CMD_QUEUE_SIZE        8
#define FRAME_BUF_SIZE        512

/* ── Polecenie sterujące ─────────────────────────────────────────────────── */
typedef struct {
    uint8_t  cmd;
    uint8_t  data[16];
    size_t   data_len;
} satel_cmd_msg_t;

/* ── Wewnętrzny stan ─────────────────────────────────────────────────────── */
static satel_state_t         s_state     = SATEL_STATE_IDLE;
static satel_state_t         s_state_prev[1] = {};  /* nieużywane tu */
static satel_panel_info_t    s_panel     = {};
static satel_state_t         s_satel_state_enum = SATEL_STATE_IDLE;

/* Mapy bitowe stanu centrali */
static satel_state_t         s_cur = {};
static satel_state_t         s_prev = {};

static SemaphoreHandle_t     s_mutex  = NULL;
static TaskHandle_t          s_task   = NULL;
static QueueHandle_t         s_cmdq   = NULL;
static satel_var_change_cb_t s_var_cb = NULL;
static int                   s_sock   = -1;

/* Dane dostępowe */
static char    s_host[64]  = {};
static uint16_t s_port     = 7094;
static uint8_t  s_pass[8]  = {};   /* zakodowane hasło INTEGRA */

/* ── Mapowanie kodu typu centrali na nazwę ─────────────────────────────── */
static const char *panel_type_name(uint8_t code) {
    switch(code) {
        case 0:  return "INTEGRA 24";
        case 1:  return "INTEGRA 32";
        case 2:  return "INTEGRA 64";
        case 3:  return "INTEGRA 128";
        case 4:  return "INTEGRA 128-WRL";
        case 132:return "INTEGRA 128-WRL LED";
        case 66: return "INTEGRA 256 PLUS";
        case 67: return "INTEGRA 256 PLUS";
        default: return "INTEGRA (unknown)";
    }
}

/* ── TCP socket helpers ───────────────────────────────────────────────────── */
static int tcp_connect(const char *host, uint16_t port) {
    struct addrinfo hints = {}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_s[8]; snprintf(port_s, sizeof(port_s), "%u", port);

    if (getaddrinfo(host, port_s, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS lookup nieudany dla %s", host);
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { freeaddrinfo(res); return -1; }

    /* Timeout połączenia */
    struct timeval tv = { .tv_sec = TCP_CONNECT_TIMEOUT_MS/1000, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "TCP connect do %s:%u nieudany: %d", host, port, errno);
        close(sock); freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    /* Ustaw timeout odbioru */
    tv.tv_sec  = TCP_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (TCP_RECV_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "TCP połączony z %s:%u", host, port);
    return sock;
}

static void tcp_close(int *sock) {
    if (*sock >= 0) { close(*sock); *sock = -1; }
}

/* ── Wyślij ramkę i odbierz odpowiedź ────────────────────────────────────── */
static esp_err_t satel_transact(int sock,
                                  const uint8_t *req_data, size_t req_len,
                                  uint8_t *resp_data, size_t *resp_len) {
    uint8_t frame[FRAME_BUF_SIZE];
    size_t  frame_len = 0;

    esp_err_t err = satel_encode_frame(req_data, req_len, frame, &frame_len);
    if (err != ESP_OK) return err;

    /* Wyślij */
    int sent = send(sock, frame, frame_len, 0);
    if (sent != (int)frame_len) {
        ESP_LOGE(TAG, "send błąd: sent=%d expected=%u", sent, (unsigned)frame_len);
        return ESP_FAIL;
    }

    /* Odbierz — czekaj na 0xFE 0x0D */
    uint8_t rbuf[FRAME_BUF_SIZE];
    size_t  rlen = 0;
    while (rlen < sizeof(rbuf) - 1) {
        int r = recv(sock, rbuf + rlen, 1, 0);
        if (r <= 0) {
            ESP_LOGW(TAG, "recv timeout/błąd: r=%d errno=%d", r, errno);
            return ESP_FAIL;
        }
        rlen++;
        /* Sprawdź koniec ramki */
        if (rlen >= 2 && rbuf[rlen-2] == 0xFE && rbuf[rlen-1] == 0x0D) break;
    }

    return satel_decode_frame(rbuf, rlen, resp_data, resp_len);
}

/* ── Odczyt mapy bitowej stanu ────────────────────────────────────────────── */
static esp_err_t poll_bitmap(int sock, uint8_t cmd,
                               uint8_t *bitmap, size_t expected_bytes) {
    uint8_t resp[64]; size_t rlen = 0;
    uint8_t req[1] = { cmd };
    esp_err_t err = satel_transact(sock, req, 1, resp, &rlen);
    if (err != ESP_OK) return err;
    /* Odpowiedź: [cmd][bitmap...] */
    if (rlen < 1 || resp[0] != cmd) return ESP_ERR_INVALID_RESPONSE;
    size_t copy = (rlen - 1 < expected_bytes) ? rlen - 1 : expected_bytes;
    memcpy(bitmap, resp + 1, copy);
    return ESP_OK;
}

/* ── Pobierz typ centrali ─────────────────────────────────────────────────── */
static void fetch_panel_info(int sock) {
    uint8_t resp[8]; size_t rlen = 0;
    uint8_t req[1] = { SATEL_CMD_INTEGRA_TYPE };
    if (satel_transact(sock, req, 1, resp, &rlen) == ESP_OK && rlen >= 2) {
        s_panel.type_code = resp[1];
        strncpy(s_panel.type_name, panel_type_name(resp[1]),
                sizeof(s_panel.type_name)-1);
        ESP_LOGI(TAG, "Centrala: %s (kod 0x%02X)", s_panel.type_name, resp[1]);
    }
    uint8_t req2[1] = { SATEL_CMD_INTEGRA_VERSION };
    if (satel_transact(sock, req2, 1, resp, &rlen) == ESP_OK && rlen >= 12) {
        /* Wersja: bajty 1..11 jako ASCII */
        memcpy(s_panel.firmware_version, resp+1, 11);
        s_panel.firmware_version[11] = '\0';
        ESP_LOGI(TAG, "Firmware: %s", s_panel.firmware_version);
    }
}

/* ── Sprawdź czy są nowe dane (NEW_DATA) ──────────────────────────────────── */
static bool check_new_data(int sock) {
    uint8_t resp[32]; size_t rlen = 0;
    uint8_t req[1] = { SATEL_CMD_NEW_DATA };
    if (satel_transact(sock, req, 1, resp, &rlen) != ESP_OK) return true;
    /* Odpowiedź: [0xFF][maska zmian...]  — bit=1 oznacza nowe dane */
    /* Dla prostoty: jeśli cokolwiek jest != 0, odśwież */
    for (size_t i = 1; i < rlen; i++)
        if (resp[i]) return true;
    return false;
}

/* ── Odśwież pełny stan centrali ─────────────────────────────────────────── */
static esp_err_t refresh_state(int sock) {
    esp_err_t err;

    /* Kopie poprzedniego stanu do porównania */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_prev, &s_cur, sizeof(s_cur));

    err = poll_bitmap(sock, SATEL_CMD_ZONES_VIOLATION,
                      s_cur.zones_violation, sizeof(s_cur.zones_violation));
    if (err != ESP_OK) { xSemaphoreGive(s_mutex); return err; }

    poll_bitmap(sock, SATEL_CMD_ZONES_TAMPER,
                s_cur.zones_tamper, sizeof(s_cur.zones_tamper));
    poll_bitmap(sock, SATEL_CMD_ZONES_ALARM,
                s_cur.zones_alarm,  sizeof(s_cur.zones_alarm));
    poll_bitmap(sock, SATEL_CMD_ZONES_BYPASSED,
                s_cur.zones_bypassed, sizeof(s_cur.zones_bypassed));
    poll_bitmap(sock, SATEL_CMD_OUTPUTS_STATE,
                s_cur.outputs_state, sizeof(s_cur.outputs_state));
    poll_bitmap(sock, SATEL_CMD_PARTITIONS_ARMED,
                s_cur.parts_armed,  sizeof(s_cur.parts_armed));
    poll_bitmap(sock, SATEL_CMD_PARTITIONS_ALARM,
                s_cur.parts_alarm,  sizeof(s_cur.parts_alarm));
    poll_bitmap(sock, SATEL_CMD_TROUBLES,
                s_cur.troubles, sizeof(s_cur.troubles));

    s_cur.last_update_s = (uint32_t)time(NULL);
    xSemaphoreGive(s_mutex);

    /* Wykryj zmiany i wywołaj callback */
    if (!s_var_cb) return ESP_OK;

    /* Wejścia — naruszenia */
    for (int n = 1; n <= SATEL_MAX_ZONES; n++) {
        bool now  = satel_bit_get(s_cur.zones_violation,  (uint8_t)n);
        bool prev = satel_bit_get(s_prev.zones_violation, (uint8_t)n);
        if (now != prev) {
            satel_variable_t v = { SATEL_VAR_ZONE_VIOLATION, (uint8_t)n,
                                   now, s_cur.last_update_s };
            s_var_cb(&v);
        }
    }
    /* Wyjścia */
    for (int n = 1; n <= SATEL_MAX_OUTPUTS; n++) {
        bool now  = satel_bit_get(s_cur.outputs_state,  (uint8_t)n);
        bool prev = satel_bit_get(s_prev.outputs_state, (uint8_t)n);
        if (now != prev) {
            satel_variable_t v = { SATEL_VAR_OUTPUT_STATE, (uint8_t)n,
                                   now, s_cur.last_update_s };
            s_var_cb(&v);
        }
    }
    /* Strefy */
    for (int n = 1; n <= SATEL_MAX_PARTS; n++) {
        bool now  = satel_bit_get(s_cur.parts_armed,  (uint8_t)n);
        bool prev = satel_bit_get(s_prev.parts_armed, (uint8_t)n);
        if (now != prev) {
            satel_variable_t v = { SATEL_VAR_PART_ARMED, (uint8_t)n,
                                   now, s_cur.last_update_s };
            s_var_cb(&v);
        }
        bool na  = satel_bit_get(s_cur.parts_alarm,  (uint8_t)n);
        bool npa = satel_bit_get(s_prev.parts_alarm, (uint8_t)n);
        if (na != npa) {
            satel_variable_t v = { SATEL_VAR_PART_ALARM, (uint8_t)n,
                                   na, s_cur.last_update_s };
            s_var_cb(&v);
        }
    }
    return ESP_OK;
}

/* ── Wyślij komendę sterującą (z hasłem) ──────────────────────────────────── */
static esp_err_t send_command_with_pass(int sock, uint8_t cmd,
                                          const uint8_t *extra, size_t extra_len) {
    /* Format: [cmd][pass 8B][extra...] */
    uint8_t data[32];
    size_t  dlen = 0;
    data[dlen++] = cmd;
    memcpy(data + dlen, s_pass, 8); dlen += 8;
    if (extra && extra_len)
        memcpy(data + dlen, extra, extra_len), dlen += extra_len;

    uint8_t resp[8]; size_t rlen = 0;
    esp_err_t err = satel_transact(sock, data, dlen, resp, &rlen);
    if (err != ESP_OK) return err;

    /* Sprawdź odpowiedź: [cmd_echo][status] */
    if (rlen >= 2 && resp[1] != 0x00) {
        ESP_LOGW(TAG, "Komenda 0x%02X: status błędu 0x%02X", cmd, resp[1]);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Push SSE state ───────────────────────────────────────────────────────── */
static void push_state(satel_state_t st) {
    const char *names[] = {"idle","connecting","ready","error","reconnect"};
    char ev[64];
    s_satel_state_enum = st;
    snprintf(ev, sizeof(ev), "{\"state\":\"%s\"}", names[st < 5 ? st : 4]);
    http_server_push_event("satel_state", ev);
}

/* ── Główny task ──────────────────────────────────────────────────────────── */
static void satel_task(void *arg) {
    uint32_t poll_tick = 0, hb_tick = 0;
    const uint32_t POLL_T = POLL_INTERVAL_MS  / 100;
    const uint32_t HB_T   = HEARTBEAT_MS      / 100;

    while (true) {
        /* ── Połącz ────────────────────────────────────────────────── */
        s_state = SATEL_STATE_CONNECTING;
        push_state(SATEL_STATE_CONNECTING);
        tcp_close(&s_sock);

        s_sock = tcp_connect(s_host, s_port);
        if (s_sock < 0) {
            ESP_LOGE(TAG, "Połączenie nieudane — retry za %ds",
                     RECONNECT_DELAY_MS/1000);
            push_state(SATEL_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        fetch_panel_info(s_sock);

        /* Wstępne pobranie pełnego stanu */
        if (refresh_state(s_sock) != ESP_OK) {
            tcp_close(&s_sock);
            push_state(SATEL_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        s_state = SATEL_STATE_READY;
        push_state(SATEL_STATE_READY);
        poll_tick = 0; hb_tick = 0;

        /* ── Pętla połączonego ──────────────────────────────────────── */
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(100));
            poll_tick++; hb_tick++;

            /* Obsłuż kolejkę komend sterujących */
            satel_cmd_msg_t msg;
            while (xQueueReceive(s_cmdq, &msg, 0) == pdTRUE) {
                if (send_command_with_pass(s_sock, msg.cmd,
                                           msg.data, msg.data_len) != ESP_OK) {
                    ESP_LOGW(TAG, "Komenda 0x%02X nieudana", msg.cmd);
                }
            }

            /* Poll stanu */
            if (poll_tick >= POLL_T) {
                poll_tick = 0;
                if (check_new_data(s_sock)) {
                    if (refresh_state(s_sock) != ESP_OK) {
                        ESP_LOGE(TAG, "refresh_state nieudany — reconnect");
                        break;
                    }
                }
            }

            /* Heartbeat */
            if (hb_tick >= HB_T) {
                hb_tick = 0;
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                /* Zlicz aktywne wejścia */
                int viol = 0;
                for (int n = 1; n <= SATEL_MAX_ZONES; n++)
                    if (satel_bit_get(s_cur.zones_violation, (uint8_t)n)) viol++;
                xSemaphoreGive(s_mutex);
                char ev[96];
                snprintf(ev, sizeof(ev),
                    "{\"state\":\"ready\",\"violations\":%d,\"panel\":\"%s\"}",
                    viol, s_panel.type_name);
                http_server_push_event("satel_heartbeat", ev);
            }
        }

        /* Reconnect */
        tcp_close(&s_sock);
        s_state = SATEL_STATE_RECONNECT;
        push_state(SATEL_STATE_RECONNECT);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

/* ─────────────────────────────────────────────────────────────────────────
   API PUBLICZNE
   ───────────────────────────────────────────────────────────────────────── */

esp_err_t satel_client_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    s_cmdq  = xQueueCreate(CMD_QUEUE_SIZE, sizeof(satel_cmd_msg_t));
    if (!s_mutex || !s_cmdq) return ESP_ERR_NO_MEM;

    /* Wczytaj zaszyfrowane dane SATEL */
    satel_creds_t creds = {};
    esp_err_t err = crypto_load_satel_creds(&creds, sizeof(creds));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Brak danych SATEL w NVS");
        return err;
    }

    strncpy(s_host, creds.host, sizeof(s_host)-1);
    s_port = (uint16_t)(creds.port ? creds.port : 7094);
    satel_encode_password(creds.password, s_pass);
    memset(&creds, 0, sizeof(creds));

    ESP_LOGI(TAG, "Klient SATEL zainicjalizowany (%s:%u)", s_host, s_port);
    return ESP_OK;
}

esp_err_t satel_client_start(void) {
    if (s_task) return ESP_ERR_INVALID_STATE;
    s_state = SATEL_STATE_IDLE;
    BaseType_t r = xTaskCreate(satel_task, "satel_client", 6144,
                                NULL, 6, &s_task);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t satel_client_stop(void) {
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    tcp_close(&s_sock);
    s_state = SATEL_STATE_IDLE;
    return ESP_OK;
}

esp_err_t satel_client_test(void) {
    ESP_LOGI(TAG, "Test połączenia SATEL %s:%u…", s_host, s_port);
    int sock = tcp_connect(s_host, s_port);
    if (sock < 0) return ESP_FAIL;
    /* Pobierz typ centrali jako test */
    uint8_t resp[8]; size_t rlen = 0;
    uint8_t req[1] = { SATEL_CMD_INTEGRA_TYPE };
    esp_err_t err = satel_transact(sock, req, 1, resp, &rlen);
    close(sock);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Test SATEL OK: centrala typ 0x%02X", rlen>1 ? resp[1] : 0);
    return err;
}

satel_state_t satel_client_get_state(void) { return s_satel_state_enum; }
const satel_state_t *satel_client_get_raw_state(void) { return &s_cur; }

bool satel_client_zone_violated(uint8_t n) {
    return satel_bit_get(s_cur.zones_violation, n); }
bool satel_client_zone_alarm   (uint8_t n) {
    return satel_bit_get(s_cur.zones_alarm, n); }
bool satel_client_zone_bypassed(uint8_t n) {
    return satel_bit_get(s_cur.zones_bypassed, n); }
bool satel_client_output_active(uint8_t n) {
    return satel_bit_get(s_cur.outputs_state, n); }
bool satel_client_part_armed   (uint8_t n) {
    return satel_bit_get(s_cur.parts_armed, n); }

/* Komendy sterujące — kolejkowanie */
static esp_err_t enqueue(uint8_t cmd, const uint8_t *data, size_t len) {
    if (!s_cmdq) return ESP_ERR_INVALID_STATE;
    satel_cmd_msg_t msg = {}; msg.cmd = cmd;
    if (data && len) { memcpy(msg.data, data, len); msg.data_len = len; }
    return (xQueueSend(s_cmdq, &msg, pdMS_TO_TICKS(500)) == pdTRUE)
            ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t satel_client_arm(uint8_t part_mask, uint8_t mode) {
    uint8_t cmds[] = { SATEL_CMD_ARM_MODE0, SATEL_CMD_ARM_MODE1,
                       SATEL_CMD_ARM_MODE2, SATEL_CMD_ARM_MODE3 };
    uint8_t data[4] = {}; data[0] = part_mask;
    return enqueue(cmds[mode & 3], data, 4);
}

esp_err_t satel_client_disarm(uint8_t part_mask) {
    uint8_t data[4] = {}; data[0] = part_mask;
    return enqueue(SATEL_CMD_DISARM, data, 4);
}

static esp_err_t output_cmd(uint8_t cmd, uint8_t num) {
    /* Format: [outputs bitmap 32B] — ustaw bit dla num */
    uint8_t bmp[32] = {};
    satel_bit_set(bmp, num, true);
    return enqueue(cmd, bmp, 32);
}

esp_err_t satel_client_output_on (uint8_t n) {
    return output_cmd(SATEL_CMD_OUTPUT_ON,  n); }
esp_err_t satel_client_output_off(uint8_t n) {
    return output_cmd(SATEL_CMD_OUTPUT_OFF, n); }

esp_err_t satel_client_ip_input_on(uint8_t n) {
    /* Wejście IP: [input_number 1B] */
    uint8_t d[1] = { n };
    return enqueue(SATEL_CMD_IP_INPUT_ON, d, 1);
}

esp_err_t satel_client_ip_input_off(uint8_t n) {
    uint8_t d[1] = { n };
    return enqueue(SATEL_CMD_IP_INPUT_OFF, d, 1);
}

const satel_panel_info_t *satel_client_get_panel_info(void) { return &s_panel; }

void satel_client_set_var_change_cb(satel_var_change_cb_t cb) { s_var_cb = cb; }

char *satel_client_state_to_json(void) {
    char *buf = (char*)malloc(1024);
    if (!buf) return NULL;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Zbierz pierwsze naruszone wejścia (max 16) dla podglądu */
    char zones_buf[256] = "[";
    int  zc = 0;
    for (int n = 1; n <= 128 && zc < 16; n++) {
        if (satel_bit_get(s_cur.zones_violation, (uint8_t)n)) {
            if (zc > 0) strcat(zones_buf, ",");
            char tmp[8]; snprintf(tmp, sizeof(tmp), "%d", n);
            strcat(zones_buf, tmp); zc++;
        }
    }
    strcat(zones_buf, "]");

    char parts_buf[32] = "[";
    int pc = 0;
    for (int n = 1; n <= 32; n++) {
        if (satel_bit_get(s_cur.parts_armed, (uint8_t)n)) {
            if (pc > 0) strcat(parts_buf, ",");
            char tmp[8]; snprintf(tmp, sizeof(tmp), "%d", n);
            strcat(parts_buf, tmp); pc++;
        }
    }
    strcat(parts_buf, "]");

    snprintf(buf, 1024,
        "{\"state\":\"%s\","
        "\"panel\":\"%s\","
        "\"firmware\":\"%s\","
        "\"violated_zones\":%s,"
        "\"armed_partitions\":%s,"
        "\"last_update\":%lu}",
        s_satel_state_enum == SATEL_STATE_READY ? "ready" : "disconnected",
        s_panel.type_name,
        s_panel.firmware_version,
        zones_buf, parts_buf,
        (unsigned long)s_cur.last_update_s);

    xSemaphoreGive(s_mutex);
    return buf;
}
