/**
 * satel_protocol.cpp — Kodowanie/dekodowanie ramek SATEL INTEGRA
 *
 * Format ramki:
 *   START:  0xFF 0xFE
 *   DATA:   bajty z escape (każdy 0xFE → 0xFE 0xF0)
 *   CRC:    2 bajty (big-endian), CRC liczony z surowych danych (przed escape)
 *   END:    0xFE 0x0D
 *
 * CRC = suma wszystkich bajtów data[] mod 65536
 */
#include "satel_protocol.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "SATEL_PROTO";

/* ── CRC ──────────────────────────────────────────────────────────────────── */
static uint16_t calc_crc(const uint8_t *data, size_t len) {
    uint16_t crc = 0x147A;
    for (size_t i = 0; i < len; i++) {
        crc = ((crc << 1) | (crc >> 15)) & 0xFFFF;  /* rotate left 1 */
        crc ^= 0xFFFF;
        crc += (crc >> 8) + data[i];
    }
    return crc;
}

/* ── Encode ───────────────────────────────────────────────────────────────── */
esp_err_t satel_encode_frame(const uint8_t *data, size_t data_len,
                               uint8_t *frame, size_t *frame_len) {
    uint16_t crc = calc_crc(data, data_len);

    size_t pos = 0;
    /* START */
    frame[pos++] = 0xFF;
    frame[pos++] = 0xFE;

    /* DATA z escape 0xFE → 0xFE 0xF0 */
    for (size_t i = 0; i < data_len; i++) {
        if (data[i] == 0xFE) { frame[pos++] = 0xFE; frame[pos++] = 0xF0; }
        else                   frame[pos++] = data[i];
    }

    /* CRC (2 bajty, big-endian) — też z escape */
    uint8_t crc_bytes[2] = { (uint8_t)(crc >> 8), (uint8_t)(crc & 0xFF) };
    for (int i = 0; i < 2; i++) {
        if (crc_bytes[i] == 0xFE) { frame[pos++] = 0xFE; frame[pos++] = 0xF0; }
        else                        frame[pos++] = crc_bytes[i];
    }

    /* END */
    frame[pos++] = 0xFE;
    frame[pos++] = 0x0D;

    *frame_len = pos;
    return ESP_OK;
}

/* ── Decode ───────────────────────────────────────────────────────────────── */
esp_err_t satel_decode_frame(const uint8_t *frame, size_t frame_len,
                               uint8_t *data, size_t *data_len) {
    /* Szukaj START */
    if (frame_len < 6) return ESP_ERR_INVALID_SIZE;
    size_t start = 0;
    while (start + 1 < frame_len &&
           !(frame[start] == 0xFF && frame[start+1] == 0xFE)) start++;
    if (start + 1 >= frame_len) {
        ESP_LOGW(TAG, "Brak znacznika START 0xFF 0xFE");
        return ESP_ERR_INVALID_RESPONSE;
    }
    start += 2;

    /* Szukaj END 0xFE 0x0D */
    size_t end = frame_len - 1;
    while (end > start &&
           !(frame[end-1] == 0xFE && frame[end] == 0x0D)) end--;
    if (end <= start) {
        ESP_LOGW(TAG, "Brak znacznika END 0xFE 0x0D");
        return ESP_ERR_INVALID_RESPONSE;
    }
    end--;  /* wskaź na 0xFE kończące */

    /* Dekoduj escape → surowe bajty */
    uint8_t raw[512]; size_t raw_len = 0;
    for (size_t i = start; i < end; i++) {
        if (frame[i] == 0xFE && i+1 < end && frame[i+1] == 0xF0) {
            raw[raw_len++] = 0xFE; i++;
        } else {
            raw[raw_len++] = frame[i];
        }
        if (raw_len >= sizeof(raw)) return ESP_ERR_INVALID_SIZE;
    }

    /* Ostatnie 2 bajty raw to CRC */
    if (raw_len < 2) return ESP_ERR_INVALID_SIZE;
    uint16_t recv_crc = ((uint16_t)raw[raw_len-2] << 8) | raw[raw_len-1];
    raw_len -= 2;

    uint16_t calc  = calc_crc(raw, raw_len);
    if (calc != recv_crc) {
        ESP_LOGE(TAG, "CRC błąd: calc=0x%04X recv=0x%04X", calc, recv_crc);
        return ESP_ERR_INVALID_CRC;
    }

    memcpy(data, raw, raw_len);
    *data_len = raw_len;
    return ESP_OK;
}

/* ── Hasło ───────────────────────────────────────────────────────────────── */
void satel_encode_password(const char *ascii_pass, uint8_t out[8]) {
    memset(out, 0xFF, 8);
    size_t len = strlen(ascii_pass);
    for (size_t i = 0; i + 1 < len && i/2 < 8; i += 2) {
        uint8_t hi = (uint8_t)(ascii_pass[i]   - '0');
        uint8_t lo = (uint8_t)(ascii_pass[i+1] - '0');
        out[i/2] = (uint8_t)((hi << 4) | lo);
    }
}
