#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* --- Komendy ---*/
#define SATEL_CMD_ZONES_VIOLATION    0x00
#define SATEL_CMD_ZONES_TAMPER       0x01
#define SATEL_CMD_ZONES_ALARM        0x02
#define SATEL_CMD_ZONES_BYPASSED     0x06
#define SATEL_CMD_OUTPUTS_STATE      0x17
#define SATEL_CMD_PARTITIONS_ARMED   0x0A
#define SATEL_CMD_PARTITIONS_ALARM   0x13
#define SATEL_CMD_ARM_MODE0          0x80
#define SATEL_CMD_DISARM             0x84
#define SATEL_CMD_CLEAR_ALARM        0x85
#define SATEL_CMD_OUTPUT_ON          0x88
#define SATEL_CMD_OUTPUT_OFF         0x89
#define SATEL_CMD_OUTPUT_SWITCH      0x91
/* --- Stałe --- */
#define SATEL_MAX_FRAME_LEN  128
#define SATEL_MAX_DATA_LEN   64
#define SATEL_MAX_ZONES      128
#define SATEL_MAX_OUTPUTS    128
#define SATEL_MAX_PARTS      32
typedef enum { SATEL_PARSE_OK=0, SATEL_PARSE_PARTIAL=1, SATEL_PARSE_ERROR=-1, SATEL_PARSE_CRC_FAIL=-2 } satel_parse_result_t;
typedef struct { uint8_t cmd; uint8_t data[SATEL_MAX_DATA_LEN]; size_t data_len; } satel_frame_t;
typedef struct {
    uint8_t zones_violated[SATEL_MAX_ZONES/8];
    uint8_t zones_tamper[SATEL_MAX_ZONES/8];
    uint8_t zones_alarm[SATEL_MAX_ZONES/8];
    uint8_t zones_bypassed[SATEL_MAX_ZONES/8];
    uint8_t outputs_state[SATEL_MAX_OUTPUTS/8];
    uint8_t partitions_armed[SATEL_MAX_PARTS/8];
    uint8_t partitions_alarm[SATEL_MAX_PARTS/8];
} satel_panel_state_t;
#ifdef __cplusplus
extern "C" {
#endif
uint16_t satel_crc(const uint8_t *data, size_t len);
size_t satel_build_frame(uint8_t cmd, const uint8_t *data, size_t dlen, const char *pass_hex, uint8_t *out, size_t olen);
satel_parse_result_t satel_parse_buffer(const uint8_t *buf, size_t blen, satel_frame_t *frame, size_t *consumed);
bool satel_bit_get(const uint8_t *arr, int num);
int  satel_bit_count(const uint8_t *arr, size_t bytes);
void satel_apply_frame(const satel_frame_t *f, satel_panel_state_t *state);
#ifdef __cplusplus
}
#endif
