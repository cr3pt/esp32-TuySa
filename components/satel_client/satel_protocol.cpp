#include "satel_protocol.h"
#include <string.h>
#include <stdlib.h>

/* CRC-16 SATEL: rotacja CRC + XOR z 0xFFFF + bajt */
uint16_t satel_crc(const uint8_t *data, size_t len) {
    uint16_t crc = 0x147A;
    for (size_t i = 0; i < len; i++) {
        crc = ((crc << 1) | (crc >> 15)) ^ 0xFFFF;
        crc += (crc >> 8) + data[i];
    }
    return crc;
}

static void hex_to_pass(const char *hex, uint8_t *pass8) {
    memset(pass8, 0xFF, 8);
    if (!hex || !*hex) return;
    int n = (int)strlen(hex); if (n > 16) n = 16;
    for (int i = 0; i < n; i += 2) {
        char b[3] = {hex[i], (i+1<n)?hex[i+1]:'0', 0};
        pass8[i/2] = (uint8_t)strtoul(b, NULL, 16);
    }
}

size_t satel_build_frame(uint8_t cmd, const uint8_t *data, size_t dlen,
                          const char *pass_hex, uint8_t *out, size_t olen) {
    uint8_t raw[SATEL_MAX_FRAME_LEN]; size_t pos = 0;
    raw[pos++] = cmd;
    if (cmd >= 0x80) { uint8_t p[8]; hex_to_pass(pass_hex,p); memcpy(raw+pos,p,8); pos+=8; }
    if (data && dlen) { if (pos+dlen > sizeof(raw)) return 0; memcpy(raw+pos,data,dlen); pos+=dlen; }
    uint16_t crc = satel_crc(raw, pos);
    raw[pos++] = (uint8_t)(crc>>8); raw[pos++] = (uint8_t)(crc&0xFF);
    size_t op = 0;
    if (olen < 4) return 0;
    out[op++]=0xFE; out[op++]=0xFE;
    for (size_t i = 0; i < pos; i++) {
        if (op+3 >= olen) return 0;
        if (raw[i]==0xFE) { out[op++]=0xFE; out[op++]=0xF0; }
        else out[op++]=raw[i];
    }
    if (op+2 >= olen) return 0;
    out[op++]=0xFE; out[op++]=0x0D;
    return op;
}

satel_parse_result_t satel_parse_buffer(const uint8_t *buf, size_t blen,
                                         satel_frame_t *frame, size_t *consumed) {
    size_t start = 0;
    while (start+1 < blen && !(buf[start]==0xFE && buf[start+1]==0xFE)) start++;
    if (start+1 >= blen) { *consumed=start; return SATEL_PARSE_PARTIAL; }
    size_t end = start+2;
    while (end+1 < blen && !(buf[end]==0xFE && buf[end+1]==0x0D)) end++;
    if (end+1 >= blen) { *consumed=start; return SATEL_PARSE_PARTIAL; }
    uint8_t raw[SATEL_MAX_FRAME_LEN]; size_t rlen=0;
    for (size_t i = start+2; i < end; i++) {
        if (rlen >= sizeof(raw)) return SATEL_PARSE_ERROR;
        if (buf[i]==0xFE && i+1<end && buf[i+1]==0xF0) { raw[rlen++]=0xFE; i++; }
        else raw[rlen++]=buf[i];
    }
    *consumed = end+2;
    if (rlen < 3) return SATEL_PARSE_ERROR;
    uint16_t got  = ((uint16_t)raw[rlen-2]<<8) | raw[rlen-1];
    uint16_t calc = satel_crc(raw, rlen-2);
    if (got != calc) return SATEL_PARSE_CRC_FAIL;
    frame->cmd = raw[0];
    frame->data_len = rlen-3; if (frame->data_len > SATEL_MAX_DATA_LEN) frame->data_len=SATEL_MAX_DATA_LEN;
    memcpy(frame->data, raw+1, frame->data_len);
    return SATEL_PARSE_OK;
}

bool satel_bit_get(const uint8_t *arr, int num) {
    if (num<1) return false;
    return (arr[(num-1)/8] >> ((num-1)%8)) & 1;
}

int satel_bit_count(const uint8_t *arr, size_t bytes) {
    int n=0; for (size_t i=0;i<bytes;i++){uint8_t b=arr[i]; while(b){n+=(b&1);b>>=1;}} return n;
}

void satel_apply_frame(const satel_frame_t *f, satel_panel_state_t *s) {
    size_t c = f->data_len;
    switch (f->cmd) {
        case SATEL_CMD_ZONES_VIOLATION: if(c>16)c=16; memcpy(s->zones_violated,f->data,c); break;
        case SATEL_CMD_ZONES_TAMPER:    if(c>16)c=16; memcpy(s->zones_tamper,  f->data,c); break;
        case SATEL_CMD_ZONES_ALARM:     if(c>16)c=16; memcpy(s->zones_alarm,   f->data,c); break;
        case SATEL_CMD_ZONES_BYPASSED:  if(c>16)c=16; memcpy(s->zones_bypassed,f->data,c); break;
        case SATEL_CMD_OUTPUTS_STATE:   if(c>16)c=16; memcpy(s->outputs_state, f->data,c); break;
        case SATEL_CMD_PARTITIONS_ARMED: if(c>4)c=4;  memcpy(s->partitions_armed,f->data,c); break;
        case SATEL_CMD_PARTITIONS_ALARM: if(c>4)c=4;  memcpy(s->partitions_alarm,f->data,c); break;
        default: break;
    }
}
