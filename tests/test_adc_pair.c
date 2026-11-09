#include "keyboard.h"
#include "adc_async.h"
#include "update_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static c77_engine engine;
static uint8_t response[64],response_pending,wire[64],incoming[64],local_status;
static unsigned local_writes,sends;
static bool satellite_fresh,relaying,queued;
static const uint8_t setting[]={0x00U,0x7dU,0,0,6U}; /* 32 kHz, six frames */
void c77_disable(c77_engine *e){e->output=0U;e->pressed_mask=0U;}
bool c77_source_fresh(const c77_engine *e,unsigned side,uint32_t now)
{(void)e;(void)now;return side==1U&&satellite_fresh;}
bool adc_scan_parse_tuning(const uint8_t p[5],uint32_t *hz,uint8_t *filter)
{*hz=32000U;*filter=p[4];return !memcmp(p,setting,5);}
bool adc_tuning_command(c77_engine *e,const uint8_t q[64],uint8_t r[64],uint32_t now,bool allow_write)
{
    (void)e;(void)now;
    assert(allow_write&&relaying&&q[3]==ADC_TUNING_COMMAND&&update_frame_valid(q));
    assert(!memcmp(q+8,setting,5));
    local_writes++;r[6]=local_status;return true;
}
void split_update_mode(bool enable){relaying=enable;}
bool split_update_send(const uint8_t p[64])
{assert(relaying&&update_frame_valid(p));memcpy(wire,p,64);sends++;return true;}
bool split_update_read(uint8_t p[64])
{if(!queued)return false;memcpy(p,incoming,64);queued=false;return true;}

#include "../src/adc_pair.inc"

static void setup(void)
{
    memset(&engine,0,sizeof engine);memset(&adc_pair,0,sizeof adc_pair);
    response_pending=local_status=0U;local_writes=sends=0U;
    satellite_fresh=true;relaying=queued=false;
}
static void request_payload(uint8_t length)
{
    uint8_t q[64]={'C','7',1U,ADC_PAIR_COMMAND,42U,0,0,length};
    if(length)memcpy(q+8,setting,5);
    update_frame_finish(q);
    assert(adc_pair_request(q,1000U));
}
static void request(void){request_payload(5U);}
static void reply(uint8_t status,bool matching,uint32_t now)
{
    memcpy(incoming,wire,64);incoming[3]|=0x80U;
    incoming[6]=status;if(!matching)incoming[12]=4U;
    update_frame_finish(incoming);queued=true;adc_pair_poll(now);
}
int main(void)
{
    setup();satellite_fresh=false;request();
    assert(!relaying&&response_pending&&response[6]==C77_NOT_READY&&!local_writes&&!sends);
    setup();engine.output=1U;request();
    assert(!relaying&&response[6]==C77_BUSY&&!local_writes);
    setup();request();adc_pair_poll(1100U);
    assert(relaying&&!response_pending&&!local_writes&&sends==1U);
    reply(0U,true,1200U);
    assert(!relaying&&response_pending&&!response[6]&&response[7]==5U&&local_writes==1U&&!adc_pair.uncertain);
    setup();request();adc_pair_poll(1100U);reply(0U,false,1200U);
    assert(!relaying&&response[6]==C77_NOT_READY&&!local_writes&&adc_pair.uncertain);
    setup();request();adc_pair_poll(1100U);reply(C77_BAD_CONFIG,true,1200U);
    assert(!relaying&&response[6]==C77_BAD_CONFIG&&!local_writes);
    setup();request();adc_pair_poll(1100U);adc_pair_poll(501000U);
    assert(!relaying&&response[6]==C77_NOT_READY&&!local_writes&&adc_pair.uncertain);
    setup();request();adc_pair_poll(1100U);adc_pair_cancel();
    assert(!relaying&&!response_pending&&!local_writes&&adc_pair.uncertain);
    setup();request();adc_pair_poll(1100U);local_status=C77_NOT_READY;reply(0U,true,1200U);
    assert(!relaying&&response[6]==C77_NOT_READY&&local_writes==1U&&adc_pair.uncertain);
    setup();request_payload(0U);adc_pair_poll(1100U);
    assert(relaying&&wire[7]==0U);
    memcpy(incoming,wire,64);incoming[3]|=0x80U;incoming[7]=5U;memcpy(incoming+8,setting,5U);
    update_frame_finish(incoming);queued=true;adc_pair_poll(1200U);
    assert(!relaying&&!response[6]&&response[7]==5U&&!memcmp(response+8,setting,5U)&&!local_writes&&!adc_pair.uncertain);
    setup();request_payload(0U);adc_pair_poll(1100U);adc_pair_poll(501000U);
    assert(!relaying&&response[6]==C77_NOT_READY&&!local_writes&&!adc_pair.uncertain);
    setup();adc_pair_watch_link(1000U);
    engine.output=engine.restore_output_pending=1U;
    satellite_fresh=false;adc_pair_watch_link(1100U);
    assert(adc_pair.uncertain&&!engine.output&&!engine.restore_output_pending);
    satellite_fresh=true;adc_pair_watch_link(1200U);
    assert(adc_pair.uncertain);
    request();adc_pair_poll(1300U);reply(0U,true,1400U);
    assert(!adc_pair.uncertain&&local_writes==1U);
    puts("ADC pair: confirmed satellite first, missing satellite/output gates, mismatched echo, timeout, cancellation and partial failure PASS");
    return 0;
}
