#pragma once
#include <stdint.h>
#include <stddef.h>
#define SATEL_CMD_ZONES_VIOLATED  0x00
#define SATEL_CMD_OUTPUT_ON       0x88
#define SATEL_CMD_OUTPUT_OFF      0x89
size_t satel_build_frame(uint8_t cmd, const uint8_t *data, size_t dlen, const char *pass, uint8_t *out, size_t olen);
int satel_parse_response(const uint8_t *buf, size_t blen, uint8_t *cmd_out, uint8_t *data_out, size_t *dlen_out);
