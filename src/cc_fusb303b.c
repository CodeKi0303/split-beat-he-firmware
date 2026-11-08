#include "cc_fusb303b.h"
#include <string.h>
/* FUSB303B/D Rev3 (2024-09), tables 11-25. No generic TI fallback. */
static const uint8_t read_regs[]={1,2,3,4,5,0x11,0x12,0x13,0x14,0x15};
typedef struct {uint8_t reg,value;bool write;} operation;
static const operation initialize[]={
    {1,0,false},{2,0,false},
    {5,0x23,true}, /* disabled; no auto sink/remedy, 150 ms debounce */
    {3,0x41,true}, /* source only; no audio, try, sink or DRP */
    {4,0x43,true}, /* default USB current; interrupts masked for setup */
    {0x0e,0,true},{0x0f,0,true},
    {0x14,0x7f,true},{0x15,0x6f,true}, /* W1C defined bits only, power blocked */
    {5,0x2b,true}, /* explicit CONTROL1.ENABLE */
    {4,0x42,true}  /* unmask interrupts after configuration */
};
static void fail(cc_fusb303b *cc,const cc_io *io,cc_error error)
{
    io->cancel(io->context);
    cc->state=CC_FAULT;cc->error=error;cc->pending=false;
    cc->enable=false;cc->allow_power=false;cc->attached=false;
}
void cc_fusb303b_reset(cc_fusb303b *cc,const cc_io *io)
{
    io->cancel(io->context);
    memset(cc,0,sizeof *cc);
}
static bool result_valid(cc_fusb303b *cc,const cc_io *io,uint8_t reg,uint8_t value)
{
    if((reg==1&&value!=0x10U)||(reg==2&&value!=3U)) {
        fail(cc,io,CC_ERROR_ID);return false;
    }
    if(cc->state==CC_READBACK &&
       ((reg==3&&(value&0x7fU)!=0x41U)||(reg==4&&value!=0x42U)||(reg==5&&value!=0x2bU))) {
        /* Detect reset/BOR or accidental mode/current changes before releasing power. */
        fail(cc,io,CC_ERROR_CONFIG);return false;
    }
    if((reg==0x11&&((value&0x80U)||(value&0x30U)==0x30U)) ||
       (reg==0x12&&(value&3U)) || (reg==0x14&&(value&0x28U)) ||
       (reg==0x15&&(value&0x6fU))) {
        fail(cc,io,CC_ERROR_STATUS);return false;
    }
    return true;
}
void cc_fusb303b_service(cc_fusb303b *cc,const cc_io *io,uint32_t now,
    bool configured,bool fault,bool id_low,bool interrupt_low)
{
    if(!configured) {
        if(cc->state!=CC_OFF) cc_fusb303b_reset(cc,io);
        return;
    }
    if(cc->state==CC_FAULT) return; /* latch until upstream unconfiguration */
    if(fault) {fail(cc,io,CC_ERROR_POWER);return;}
    if(!id_low) {cc->allow_power=false;cc->attached=false;}
    if(cc->state==CC_OFF) {
        cc->enable=true;cc->state=CC_STARTING;cc->started_at=now;return;
    }
    if(cc->state==CC_STARTING) {
        if((uint32_t)(now-cc->started_at)<100000U) return; /* tEN maximum */
        cc->state=CC_INIT;cc->step=0;
    }
    if(cc->state==CC_READY) {
        if((uint32_t)(now-cc->checked_at)<10000U&&!interrupt_low) return;
        cc->state=CC_READBACK;cc->step=0;
    }
    /* A late/stalled main loop cannot keep an old positive attach decision. */
    if(cc->allow_power&&(uint32_t)(now-cc->checked_at)>20000U) {
        fail(cc,io,CC_ERROR_TIMEOUT);return;
    }
    if(cc->pending) {
        cc_io_result result=io->poll(io->context);
        if(result==CC_IO_ERROR) {fail(cc,io,CC_ERROR_BUS);return;}
        if(result==CC_IO_BUSY) {
            if((uint32_t)(now-cc->transaction_at)>=5000U) fail(cc,io,CC_ERROR_TIMEOUT);
            return;
        }
        cc->pending=false;
        if(cc->state==CC_INIT) {
            if(!initialize[cc->step].write &&
               !result_valid(cc,io,initialize[cc->step].reg,cc->value)) return;
            if(++cc->step==sizeof initialize/sizeof initialize[0]) {cc->state=CC_READBACK;cc->step=0;}
        } else if(cc->state==CC_READBACK) {
            if(!result_valid(cc,io,read_regs[cc->step],cc->value)) return;
            cc->registers[cc->step]=cc->value;
            if(++cc->step==sizeof read_regs) {cc->state=CC_CLEAR;cc->step=0;}
        } else if(cc->state==CC_CLEAR && ++cc->step==2U) {
            uint8_t status=cc->registers[5],type=cc->registers[7]&0x7fU;
            uint8_t orientation=status&0x30U;
            /* TYPE.SOURCE denotes OUR source role. Refuse debug/audio/active cable.
             * Do not require VBUSOK before powering a sink: that is circular. */
            cc->attached=(status&1U)&&type==8U&&(orientation==0x10U||orientation==0x20U);
            cc->allow_power=cc->attached&&id_low;
            cc->checked_at=now;cc->state=CC_READY;return;
        }
    }
    bool write=false;uint8_t reg;
    if(cc->state==CC_INIT) {
        operation op=initialize[cc->step];reg=op.reg;write=op.write;cc->value=op.value;
    } else if(cc->state==CC_READBACK) {reg=read_regs[cc->step];cc->value=0;}
    else {reg=(uint8_t)(0x14U+cc->step);write=true;cc->value=cc->registers[8U+cc->step]&(cc->step?0x6fU:0x7fU);}
    if(!io->start(io->context,write,reg,&cc->value)) {fail(cc,io,CC_ERROR_BUS);return;}
    cc->transaction_at=now;cc->pending=true;
}
