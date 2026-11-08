/* Include the production codec so the exhaustive test exercises its private
 * byte update directly, without exposing a test-only production API. */
#include <assert.h>
#include <stdio.h>
#include "../src/split_codec.c"

static uint16_t reference_byte(uint16_t value,uint8_t byte)
{
    value^=(uint16_t)byte<<8;
    for(unsigned bit=0;bit<8;bit++)
        value=(uint16_t)((value<<1)^((value&0x8000U)?0x1021U:0U));
    return value;
}
static uint16_t reference_crc(const uint8_t *bytes,unsigned count)
{
    uint16_t value=0xffffU;
    for(unsigned i=0;i<count;i++)value=reference_byte(value,bytes[i]);
    return value;
}
static void exhaustive_byte_equivalence(void)
{
    for(uint32_t state=0;state<65536U;state++)
        for(unsigned byte=0;byte<256U;byte++)
            assert(crc_byte((uint16_t)state,(uint8_t)byte)==
                   reference_byte((uint16_t)state,(uint8_t)byte));
    assert(crc((const uint8_t *)"123456789",9)==0x29b1U);
    assert(crc(NULL,0)==0xffffU);
}
static void test_wire_and_corruption(void)
{
    uint8_t request[8],reply[32],broken[32],output[32];
    c77_sample_t sample={0},decoded;
    sample.valid=1;sample.sequence=0x12345678U;sample.digital=7;
    sample.encoders[0]=0xabcdU;sample.encoders[1]=0x9876U;
    for(unsigned i=0;i<7;i++)sample.raw[i]=(uint16_t)(i*600U);
    c77_split_reply(reply,0x5312U,&sample);
    c77_split_request(request,0x7312U);
    assert(get16(request+6)==reference_crc(request,6));
    assert(get16(reply+30)==reference_crc(reply,30));
    assert(c77_split_decode(reply,&decoded));
    assert(decoded.sequence==sample.sequence&&decoded.valid);
    for(unsigned bit=0;bit<sizeof reply*8U;bit++) {
        memcpy(broken,reply,sizeof reply);broken[bit/8U]^=(uint8_t)(1U<<(bit%8U));
        assert(!c77_split_decode(broken,&decoded));
        c77_split_parser_t parser={0};parser.reply=1;
        for(unsigned i=0;i<sizeof broken;i++)
            assert(!c77_split_feed(&parser,broken[i],output));
        bool got=false;
        for(unsigned i=0;i<sizeof reply;i++)
            if(c77_split_feed(&parser,reply[i],output))got=true;
        assert(got&&!memcmp(output,reply,sizeof reply));
    }
}
int main(void)
{
    exhaustive_byte_equivalence();test_wire_and_corruption();
    puts("split CRC: all 16777216 state/byte pairs, wire vectors, corruption and resync passed");
    return 0;
}
