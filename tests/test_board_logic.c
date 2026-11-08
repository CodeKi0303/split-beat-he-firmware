#include "input_scan.h"
#include "split_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned feed(c77_split_parser_t *p,const uint8_t *bytes,unsigned n,uint8_t *out)
{unsigned frames=0;while(n--) frames+=c77_split_feed(p,*bytes++,out);return frames;}
static uint16_t independent_crc(const uint8_t *p,unsigned n)
{
    uint16_t c=0xffff;
    while(n--) {c^=(uint16_t)(*p++<<8);for(unsigned b=0;b<8;b++) c=(uint16_t)((c&0x8000)?(c<<1)^0x1021:c<<1);}
    return c;
}
static void crcfix(uint8_t *p)
{uint16_t c=independent_crc(p,30);p[30]=(uint8_t)c;p[31]=(uint8_t)(c>>8);}
int main(void)
{
    c77_debounce_t b={0};
    assert(c77_debounce(&b,1,1000)==0);
    assert(c77_debounce(&b,0,1200)==0);
    assert(c77_debounce(&b,1,1400)==0);
    assert(c77_debounce(&b,1,6399)==0);
    assert(c77_debounce(&b,1,6400)==1);
    assert(c77_debounce(&b,0,0xfffffff0U)==1);
    assert(c77_debounce(&b,0,0xfffffff0U+4999U)==1);
    assert(c77_debounce(&b,0,0xfffffff0U+5000U)==0);

    c77_quadrature_t q={0};
    const uint8_t forward[]={3,1,0,2,3};
    const uint8_t reverse[]={3,2,0,1,3};
    for(unsigned i=0;i<5;i++) c77_quadrature(&q,forward[i]);
    assert(q.total==1&&q.invalid==0);
    for(unsigned i=0;i<5;i++) c77_quadrature(&q,reverse[i]);
    assert(q.total==0&&q.invalid==0);
    for(unsigned i=0;i<10;i++) {c77_quadrature(&q,1);c77_quadrature(&q,3);}
    assert(q.total==0);
    c77_quadrature(&q,0);assert(q.invalid==1&&q.total==0);
    q.total=65535;q.previous=3;q.partial=0;
    for(unsigned i=0;i<5;i++) c77_quadrature(&q,forward[i]);
    assert(q.total==0); /* Wrap is intentional cumulative uint16 transport. */

    c77_sample_t source={.raw={0,1,1023,2048,3000,4094,4095},.digital=7,.valid=1,
        .sequence=0xffffffffU,.encoders={65535,32768}},decoded;
    uint8_t good[32],bad[32],out[32],request[8];
    c77_split_reply(good,0xbeef,&source);
    /* Private left-filter data is neither serialized nor inherited by a
     * decoder called with an uninitialized destination. S7 remains32 bytes. */
    source.hall_filtered_valid=1U;
    for(unsigned i=0;i<7;i++) source.hall_filtered[i]=1234U;
    c77_split_reply(bad,0xbeef,&source);assert(!memcmp(good,bad,sizeof good));
    memset(&decoded,0xa5,sizeof decoded);
    assert(independent_crc(good,30)==(uint16_t)(good[30]|(good[31]<<8)));
    assert(c77_split_decode(good,&decoded));
    assert(!decoded.hall_filtered_valid);
    for(unsigned i=0;i<7;i++) assert(decoded.hall_filtered[i]==0U);
    assert(decoded.valid&&decoded.sequence==source.sequence&&decoded.digital==7);
    assert(!memcmp(source.raw,decoded.raw,sizeof source.raw));
    assert(!memcmp(source.encoders,decoded.encoders,sizeof source.encoders));
    for(unsigned bit=0;bit<256;bit++) {
        memcpy(bad,good,32);bad[bit/8]^=(uint8_t)(1U<<(bit%8));
        assert(!c77_split_decode(bad,&decoded));
        c77_split_parser_t p={.reply=1};
        assert(feed(&p,bad,32,out)==0);
        assert(feed(&p,good,32,out)==1);
        assert(!memcmp(out,good,32));
    }
    for(unsigned used=1;used<32;used++) {
        c77_split_parser_t p={.reply=1};
        assert(feed(&p,good,used,out)==0);
        assert(feed(&p,good,32,out)==1); /* Truncated packet followed by full packet. */
    }
    memcpy(bad,good,32);bad[6]=0;bad[7]=0x10;crcfix(bad);
    assert(!c77_split_decode(bad,&decoded)); /* CRC-valid ADC out of range. */
    memcpy(bad,good,32);bad[24]=0x80;crcfix(bad);assert(!c77_split_decode(bad,&decoded));
    memcpy(bad,good,32);bad[25]=8;crcfix(bad);assert(!c77_split_decode(bad,&decoded));
    c77_split_request(request,0);c77_split_parser_t rp={0};
    assert(feed(&rp,request,8,out)==1&&c77_split_sequence(out)==0);
    c77_split_request(request,65535);assert(feed(&rp,request,8,out)==1&&c77_split_sequence(out)==65535);
    c77_split_parser_t junk={.reply=1};
    for(unsigned i=0;i<100000;i++) assert(!c77_split_feed(&junk,0xaa,out));
    assert(feed(&junk,good,32,out)==1);
    puts("PASS: debounce/bounce/wrap, quadrature invalid edges, split CRC corruption/ranges/resynchronization");
    return 0;
}
