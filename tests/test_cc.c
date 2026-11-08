#include "cc_fusb303b.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    uint8_t regs[32],reg,*value;
    unsigned starts,cancels,fail_at,error_at,ignore_at;
    bool pending,write,stall;
} fake_bus;
static fake_bus bus;
static cc_fusb303b cc;
static uint32_t now;
static bool configured,fault,id_low,irq_low;
static bool start(void *context,bool write,uint8_t reg,uint8_t *value)
{
    fake_bus *b=context;
    assert(!b->pending);assert(reg<32U);
    ++b->starts;if(b->starts==b->fail_at) return false;
    b->reg=reg;b->write=write;b->value=value;b->pending=true;return true;
}
static cc_io_result poll(void *context)
{
    fake_bus *b=context;assert(b->pending);
    if(b->stall) return CC_IO_BUSY;
    b->pending=false;
    if(b->starts==b->error_at) return CC_IO_ERROR;
    if(b->write) {
        if(b->starts!=b->ignore_at) {
            if(b->reg==0x14U||b->reg==0x15U) b->regs[b->reg]&=(uint8_t)~*b->value;
            else b->regs[b->reg]=*b->value;
        }
    } else *b->value=b->regs[b->reg];
    return CC_IO_OK;
}
static void cancel(void *context)
{fake_bus *b=context;b->pending=false;++b->cancels;}
static const cc_io io={&bus,start,poll,cancel};
static void step(uint32_t advance)
{
    now+=advance;
    cc_fusb303b_service(&cc,&io,now,configured,fault,id_low,irq_low);
    if(cc.allow_power) {
        assert(configured&&!fault&&id_low&&cc.enable&&cc.attached);
        assert(cc.error==CC_ERROR_NONE);
    }
}
static void reset_model(uint32_t start_time)
{
    memset(&bus,0,sizeof bus);memset(&cc,0,sizeof cc);
    bus.regs[1]=0x10;bus.regs[2]=3;bus.regs[3]=0x49;bus.regs[4]=0x43;bus.regs[5]=0x23;
    bus.regs[0x11]=0x11; /* ATTACH + CC1, VBUSOK deliberately false */
    bus.regs[0x13]=8;now=start_time;configured=false;fault=false;id_low=true;irq_low=false;
    cc_fusb303b_reset(&cc,&io);
}
static void until_ready(void)
{
    configured=true;
    for(unsigned i=0;i<250U&&cc.state!=CC_READY&&cc.state!=CC_FAULT;++i) step(600);
    assert(cc.state==CC_READY);
}
static void until_fault(void)
{
    for(unsigned i=0;i<300U&&cc.state!=CC_FAULT;++i) step(600);
    assert(cc.state==CC_FAULT&&!cc.allow_power&&!cc.enable);
    unsigned starts=bus.starts;
    for(unsigned i=0;i<10;++i) step(1000);
    assert(cc.state==CC_FAULT&&bus.starts==starts); /* no auto retry */
}
int main(void)
{
    reset_model(0);step(200000);assert(!cc.enable&&!cc.allow_power&&!bus.starts);
    configured=true;step(0);assert(cc.enable&&!cc.allow_power);
    step(99999);assert(!bus.starts);step(1);assert(bus.starts==1&&!cc.allow_power);
    until_ready();assert(cc.allow_power);assert(bus.regs[3]==0x41&&bus.regs[4]==0x42&&bus.regs[5]==0x2b);
    /* Every initialization/snapshot/clear transfer must fail closed on NACK. */
    unsigned initial_transfers=bus.starts;
    for(unsigned n=1;n<=initial_transfers;++n) {
        reset_model(0);configured=true;bus.fail_at=n;until_fault();
        reset_model(0);configured=true;bus.error_at=n;until_fault();
    }
    /* Silent rejected writes are detected by actual register readback. */
    const unsigned required_writes[]={4,10,11};
    for(unsigned i=0;i<sizeof required_writes/sizeof required_writes[0];++i) {
        reset_model(0);configured=true;bus.ignore_at=required_writes[i];until_fault();
        assert(cc.error==CC_ERROR_CONFIG);
    }
    for(unsigned reg=1;reg<=5;++reg) {
        reset_model(0);until_ready();bus.regs[reg]^=1;until_fault();
    }
    /* Reset/BOR has no dedicated status bit: enable/mode readback catches it. */
    reset_model(0);until_ready();bus.regs[5]=0x23;until_fault();
    configured=false;step(0);assert(cc.state==CC_OFF&&cc.error==CC_ERROR_NONE);
    until_ready();assert(cc.allow_power);
    /* Reject sink, debug, audio, active cable, orientation faults and autosink. */
    const uint8_t bad_types[]={0,1,2,4,12,16,32,64,72};
    for(unsigned i=0;i<sizeof bad_types;++i) {
        reset_model(0);bus.regs[0x13]=bad_types[i];until_ready();assert(!cc.allow_power);
    }
    reset_model(0);bus.regs[0x11]=0x31;configured=true;until_fault();
    reset_model(0);bus.regs[0x11]=0x91;configured=true;until_fault();
    reset_model(0);bus.regs[0x12]=2;configured=true;until_fault();
    reset_model(0);until_ready();bus.regs[0x14]=0x20;irq_low=true;until_fault();
    reset_model(0);until_ready();bus.regs[0x15]=0x40;irq_low=true;until_fault();
    /* Physical ID drops power immediately without waiting for an I2C sample. */
    reset_model(0);until_ready();id_low=false;step(0);assert(!cc.allow_power);
    reset_model(0);until_ready();fault=true;step(0);assert(cc.error==CC_ERROR_POWER&&!cc.allow_power);
    /* Upstream suspend/disconnect cancels even an in-flight transfer. */
    reset_model(0);until_ready();step(10000);assert(bus.pending);
    configured=false;step(0);assert(!bus.pending&&!cc.allow_power&&!cc.enable);
    /* Dead bus, stale attach, and timer wrap have bounded behavior. */
    reset_model(0);until_ready();bus.stall=true;step(10000);step(5000);
    assert(cc.error==CC_ERROR_TIMEOUT&&!cc.allow_power);
    reset_model(0);until_ready();step(20001);assert(cc.error==CC_ERROR_TIMEOUT&&!cc.allow_power);
    reset_model(0xffff0000U);until_ready();assert(cc.allow_power);
    for(unsigned i=0;i<100;++i) step(600);
    assert(cc.state!=CC_FAULT&&cc.allow_power);
    puts("FUSB303B source policy PASS: startup, readback, bus failures, reset, unsafe attach, power budget, timeout, wrap. No hardware qualification.");
    return 0;
}
