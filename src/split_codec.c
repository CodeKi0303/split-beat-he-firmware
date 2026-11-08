#include "split_codec.h"
#include <string.h>
static uint16_t get16(const uint8_t *p) {return p[0]|((uint16_t)p[1]<<8);}
static void put16(uint8_t *p,uint16_t x) {p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static uint32_t get32(const uint8_t *p) {return get16(p)|((uint32_t)get16(p+2)<<16);}
static void put32(uint8_t *p,uint32_t x) {put16(p,(uint16_t)x);put16(p+2,(uint16_t)(x>>16));}
static inline uint16_t crc_byte(uint16_t value,uint8_t byte)
{
    /* CRC-CCITT (0x1021), MSB first: fold all eight polynomial steps at once.
     * Table-free, with the same result for every 16-bit state and input byte. */
    uint32_t x=(value>>8)^byte;
    x^=x>>4;
    return (uint16_t)(((uint32_t)value<<8)^(x<<12)^(x<<5)^x);
}
static uint16_t crc(const uint8_t *p,unsigned count)
{
    uint16_t c=0xffffU;
    while(count--)c=crc_byte(c,*p++);
    return c;
}
uint16_t c77_split_sequence(const uint8_t *p) {return get16(p+4);}
void c77_split_request(uint8_t p[8],uint16_t sequence)
{
    p[0]='S';p[1]='7';p[2]=1;p[3]=0x10;put16(p+4,sequence);put16(p+6,crc(p,6));
}
void c77_split_reply(uint8_t p[32],uint16_t sequence,const c77_sample_t *s)
{
    memset(p,0,32);p[0]='S';p[1]='7';p[2]=1;p[3]=0x20;put16(p+4,sequence);
    for(unsigned i=0;i<7;i++) put16(p+6+i*2,s->raw[i]);
    put32(p+20,s->sequence);p[24]=(s->valid?1U:0U)|(s->errors?2U:0U);p[25]=s->digital;
    put16(p+26,s->encoders[0]);put16(p+28,s->encoders[1]);put16(p+30,crc(p,30));
}
bool c77_split_decode(const uint8_t p[32],c77_sample_t *s)
{
    if(p[0]!='S'||p[1]!='7'||p[2]!=1||p[3]!=0x20||get16(p+30)!=crc(p,30)
       ||(p[24]&~3U)||(p[25]&~7U)) return false;
    memset(s,0,sizeof *s);
    for(unsigned i=0;i<7;i++) {s->raw[i]=get16(p+6+i*2);if(s->raw[i]>4095) return false;}
    s->sequence=get32(p+20);s->valid=(p[24]&1U)&&s->sequence;s->errors=(p[24]>>1)&1U;
    s->digital=p[25];s->encoders[0]=get16(p+26);s->encoders[1]=get16(p+28);
    return true;
}
bool c77_split_feed(c77_split_parser_t *p,uint8_t byte,uint8_t out[32])
{
    unsigned length=p->reply?32U:8U;
    p->data[p->used++]=byte;
    for(;;) {
        bool bad=p->data[0]!='S'||(p->used>1&&p->data[1]!='7')||
            (p->used>2&&p->data[2]!=1)||(p->used>3&&p->data[3]!=(p->reply?0x20:0x10));
        if(!bad&&p->used<length) return false;
        if(!bad&&get16(p->data+length-2)==crc(p->data,length-2)) {
            memcpy(out,p->data,length);p->used=0;return true;
        }
        if(!bad) p->crc_errors++; else p->framing_errors++;
        p->used--;memmove(p->data,p->data+1,p->used);
        if(!p->used) return false;
    }
}
