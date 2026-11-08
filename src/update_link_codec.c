#include "update_link.h"
#include <string.h>
uint16_t update_frame_crc(const uint8_t *p,unsigned n){
    uint16_t c=0xffffu;
    while(n--){c^=(uint16_t)*p++<<8;for(unsigned b=0;b<8;b++)c=(uint16_t)((c<<1)^((c&0x8000u)?0x1021u:0));}
    return c;
}
void update_frame_finish(uint8_t p[64]){uint16_t c=update_frame_crc(p,62);p[62]=(uint8_t)c;p[63]=(uint8_t)(c>>8);}
bool update_frame_valid(const uint8_t p[64]){
    if(p[0]!='C'||p[1]!='7'||p[2]!=1||!(p[4]|p[5])||p[7]>54||
       update_frame_crc(p,62)!=(uint16_t)(p[62]|((uint16_t)p[63]<<8)))return false;
    for(unsigned i=8+p[7];i<62;i++)if(p[i])return false;
    return true;
}
bool update_frame_feed(update_frame_parser *p,uint8_t byte,uint8_t out[64]){
    p->bytes[p->used++]=byte;
    for(;;){
        bool bad=p->bytes[0]!='C'||(p->used>1&&p->bytes[1]!='7')||(p->used>2&&p->bytes[2]!=1);
        if(!bad&&p->used<64)return false;
        if(!bad&&update_frame_valid(p->bytes)){memcpy(out,p->bytes,64);p->used=0;return true;}
        p->errors++;p->used--;memmove(p->bytes,p->bytes+1,p->used);
        if(!p->used)return false;
    }
}
