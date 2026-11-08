/* Exercise the diagnostic wire handler with production CRC/serialization. */
#include "adc_trace.h"
#include "keyboard.h"
#include "update_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t response[64],response_pending;
static c77_engine engine;
#include "../src/adc_trace.inc"

static uint16_t word(const uint8_t *p) {return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}

int main(void)
{
    uint8_t request[64]={'C','7',1,ADC_TRACE_COMMAND,0x34,0x12,0,2,5,6};
    update_frame_finish(request);
    engine.stored_valid=1;
    assert(trace_request(request,12345) && adc_trace.pending && !response_pending);
    assert(adc_trace.requested_us==12345);
    c77_sample_t sample={0};
    sample.sequence=0x23456789U;sample.timestamp_us=0xabcdef12U;
    adc_trace_history history={0};
    for(unsigned k=0;k<2;k++){
        for(unsigned i=0;i<8;i++)history.raw[k][i]=(uint16_t)(2100U+k*100U+i);
        sample.hall_filtered[k+5]=(uint16_t)(2104U+k*100U);
        engine.filtered[k+5]=(uint16_t)(2103U+k*100U);
        engine.travel[k+5]=(uint16_t)(50U+k*50U);
    }
    engine.pressed_mask=1U<<6;
    trace_response(C77_OK,&sample,&history);
    assert(response_pending && !adc_trace.pending && update_frame_valid(response));
    assert(response[3]==0xc4 && word(response+4)==0x1234 && response[6]==0 && response[7]==54);
    assert(word(response+8)==0x6789 && word(response+10)==0x2345);
    assert(word(response+12)==0xef12 && word(response+14)==0xabcd);
    assert(response[16]==5 && response[17]==6);
    for(unsigned k=0;k<2;k++){
        const uint8_t *row=response+18+22*k;
        for(unsigned i=0;i<8;i++)assert(word(row+2*i)==history.raw[k][i]);
        assert(word(row+16)==sample.hall_filtered[k+5]);
        assert(word(row+18)==engine.filtered[k+5]);
        assert(word(row+20)==(uint16_t)(engine.travel[k+5]|(k?0x8000U:0U)));
    }
    engine.output=1;
    assert(trace_request(request,0) && !adc_trace.pending && response[6]==C77_BUSY);
    engine.output=0;
    request[9]=5;update_frame_finish(request);
    assert(trace_request(request,0) && !adc_trace.pending && response[6]==C77_BAD_INDEX);
    request[9]=6;update_frame_finish(request);request[63]^=1;
    assert(trace_request(request,0) && !adc_trace.pending && response[6]==C77_BAD_PACKET);
    update_frame_finish(request);
    assert(trace_request(request,0) && adc_trace.pending);
    trace_response(C77_NOT_READY,NULL,NULL);
    assert(!adc_trace.pending && response[6]==C77_NOT_READY && response[7]==0 && update_frame_valid(response));
    for(unsigned i=8;i<62;i++)assert(response[i]==0);
    puts("C86 trace: coherent packet layout, CRC, safety gates and unavailable reply PASS");
    return 0;
}
