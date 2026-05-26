#include "satel_protocol.h"
#include <string.h>
size_t satel_build_frame(uint8_t cmd,const uint8_t *data,size_t dlen,const char *pass,uint8_t *out,size_t olen){ (void)pass; if(olen<dlen+4) return 0; out[0]=0xFE; out[1]=0xFE; out[2]=cmd; if(data&&dlen) memcpy(out+3,data,dlen); out[3+dlen]=0xFE; out[4+dlen-1]=0x0D; return dlen+4; }
int satel_parse_response(const uint8_t *buf,size_t blen,uint8_t *cmd_out,uint8_t *data_out,size_t *dlen_out){ if(blen<3) return -1; *cmd_out=buf[2]; if(data_out&&dlen_out&&*dlen_out>0){ size_t n=blen-4; if(n>*dlen_out)n=*dlen_out; memcpy(data_out,buf+3,n); *dlen_out=n; } return 0; }
