/**
 * Minimalny serwer DNS UDP do obsługi Captive Portal.
 * Odpowiada na każde zapytanie DNS typu A adresem IP podanym przy starcie.
 * Nie obsługuje innych typów rekordów – to wystarczy dla przekierowania.
 */
#include "dns_server.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG      = "DNS";
static TaskHandle_t s_task  = NULL;
static int          s_sock  = -1;
static uint32_t     s_redir = 0;

#define DNS_PORT  53
#define DNS_BUFLEN 512

/* Nagłówek DNS (RFC 1035) */
#pragma pack(push, 1)
typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;
#pragma pack(pop)

static void dns_task(void *arg) {
    uint8_t buf[DNS_BUFLEN];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    while (true) {
        int len = recvfrom(s_sock, buf, DNS_BUFLEN, 0,
                           (struct sockaddr *)&src, &src_len);
        if (len < (int)sizeof(dns_header_t)) continue;

        /* Zbuduj odpowiedź: przepisz nagłówek, ustaw flagi odpowiedzi */
        uint8_t resp[DNS_BUFLEN];
        memcpy(resp, buf, len);
        dns_header_t *hdr = (dns_header_t *)resp;
        hdr->flags   = htons(0x8180);   /* QR=1, AA=1, RD=1, RA=1 */
        hdr->ancount = htons(1);

        /* Dołącz rekord odpowiedzi A */
        int rlen = len;
        /* Wskaźnik kompresji do nazwy w pytaniu (offset=12) */
        resp[rlen++] = 0xC0;
        resp[rlen++] = 0x0C;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;  /* type A */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;  /* class IN */
        /* TTL = 10 s */
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x0A;
        /* RDLENGTH = 4 */
        resp[rlen++] = 0x00; resp[rlen++] = 0x04;
        /* RDATA = adres IP */
        memcpy(&resp[rlen], &s_redir, 4);
        rlen += 4;

        sendto(s_sock, resp, rlen, 0,
               (struct sockaddr *)&src, src_len);
    }
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(uint32_t redirect_ip) {
    s_redir = redirect_ip;
    s_sock  = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) { ESP_LOGE(TAG, "socket() failed"); return ESP_FAIL; }

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(DNS_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(s_sock);
        return ESP_FAIL;
    }

    xTaskCreate(dns_task, "dns_srv", 4096, NULL, 5, &s_task);
    ESP_LOGI(TAG, "Serwer DNS uruchomiony (redirect → %lu)", (unsigned long)redirect_ip);
    return ESP_OK;
}

esp_err_t dns_server_stop(void) {
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    return ESP_OK;
}
