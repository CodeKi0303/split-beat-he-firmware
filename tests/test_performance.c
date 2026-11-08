/* Exercise the real diagnostics serializer with read-only snapshot mocks. */
#include "performance.h"
#include "update_link.h"
#include "split_link_diag.h"
#include "usb_device.h"
#include "adc_scan_private.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned usb_reads,adc_reads,link_reads,time_reads;
static usb_keyboard_stats_t usb={11,12,13,14};
static split_adc_snapshot adc;
static split_link_stats_t link={101,102,103,104,105,106,107,108,109,110,111,112,3,201,202,203,204,205};
volatile uint32_t diagnostic_cc_state=9,diagnostic_cc_error=2;
uint32_t board_micros(void){time_reads++;return 0xFEDCBA98U;}
void usb_keyboard_stats(usb_keyboard_stats_t *out){usb_reads++;*out=usb;}
void adc_scan_stats(split_adc_snapshot *out){adc_reads++;*out=adc;}
void split_link_stats(split_link_stats_t *out){link_reads++;*out=link;}

static void query(uint8_t q[64],uint8_t op){
    memset(q,0,64);q[0]='C';q[1]='7';q[2]=1;q[3]=op;q[4]=0x34;q[5]=0x12;
    update_frame_finish(q);
}
static uint32_t word(const uint8_t *p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void verify(uint8_t op,const uint32_t expected[13]){
    uint8_t q[64],before[64],r[64];query(q,op);memcpy(before,q,64);
    split_link_stats_t saved_link=link;
    usb_keyboard_stats_t saved_usb=usb;
    split_adc_snapshot saved_adc=adc;
    assert(performance_command(q,r));assert(!memcmp(q,before,64));
    assert(update_frame_valid(r));assert(r[3]==(uint8_t)(op|0x80U));
    assert(r[4]==0x34&&r[5]==0x12&&r[6]==0&&r[7]==52);
    for(unsigned i=0;i<13;i++)assert(word(r+8+4*i)==expected[i]);
    assert(!memcmp(&link,&saved_link,sizeof link));
    assert(!memcmp(&usb,&saved_usb,sizeof usb));
    assert(!memcmp(&adc,&saved_adc,sizeof adc));
}
int main(void){
    adc.sample_seq=21;adc.errors=22;adc.overruns=23;adc.late_irqs=24;
    adc.period_min_cycles=25;adc.period_max_cycles=26;adc.irq_max_cycles=27;
    const uint32_t old_schema[13]={0xFEDCBA98U,11,12,13,14,21,22,23,24,25,26,27,SCAN_TARGET_HZ};
    const uint32_t link_schema[13]={0xFEDCBA98U,101,102,103,104,105,106,107,108,109,110,111,112};
    adc.running=0;adc.dma_errors=31;adc.init_error=32;adc.stop_reason=2;
    adc.stop_phase_cycles=2600;adc.stop_dma_flags=0x30;adc.stop_ndtr1=4;adc.stop_ndtr2=3;
    const uint32_t diagnostic_schema[13]={0,21,22,23,31,24,32,2,2600,0x30,4,3,SCAN_TARGET_HZ};
    const uint32_t empty_schedule[13]={0xFEDCBA98U,0,0,0,0,9,2,3,201,202,203,204,205};
    verify(0x43,empty_schedule);
    performance_loop_begin(UINT32_MAX-8U);
    const uint32_t first_schedule[13]={0xFEDCBA98U,1,0,0,0,9,2,3,201,202,203,204,205};
    verify(0x43,first_schedule); /* Initial timestamp is not a giant gap. */
    performance_loop_begin(5U);performance_loop_begin(10U); /* Wrapped gap14, then5. */
    performance_sample_update(0,12);performance_sample_update(0,12);performance_sample_update(0,13);
    performance_sample_update(1,UINT32_MAX);performance_sample_update(1,UINT32_MAX);
    performance_sample_update(1,1);performance_sample_update(1,0);performance_sample_update(2,1);
    const uint32_t schedule_schema[13]={0xFEDCBA98U,3,14,2,2,9,2,3,201,202,203,204,205};
    verify(0x43,schedule_schema);verify(0x43,schedule_schema); /* Read does not reset counters. */
    usb_reads=adc_reads=link_reads=time_reads=0;
    verify(0x40,old_schema);assert(usb_reads==1&&adc_reads==1&&link_reads==0&&time_reads==1);
    verify(0x41,link_schema);assert(usb_reads==1&&adc_reads==1&&link_reads==1&&time_reads==2);
    verify(0x42,diagnostic_schema);assert(usb_reads==1&&adc_reads==2&&link_reads==1&&time_reads==2);
    verify(0x43,schedule_schema);assert(usb_reads==1&&adc_reads==2&&link_reads==2&&time_reads==3);
    uint8_t q[64],r[64];
    for(unsigned op=0;op<256;op++){
        if(op==0x40||op==0x41||op==0x42||op==0x43)continue;
        query(q,(uint8_t)op);memset(r,0xA5,64);
        assert(!performance_command(q,r));
        for(unsigned i=0;i<64;i++)assert(r[i]==0xA5);
    }
    for(unsigned op=0x40;op<=0x43;op++)for(unsigned bad=0;bad<4;bad++){
        query(q,(uint8_t)op);
        if(bad==0){q[7]=1;q[8]=0xFF;update_frame_finish(q);}
        if(bad==1){q[6]=1;update_frame_finish(q);}
        if(bad==2){q[2]=2;update_frame_finish(q);}
        if(bad==3)q[63]^=1;
        assert(performance_command(q,r));assert(r[6]==2&&r[7]==0);
        for(unsigned i=8;i<60;i++)assert(r[i]==0);
    }
    assert(usb_reads==1&&adc_reads==2&&link_reads==2&&time_reads==3);
    puts("Performance 0x40 compatibility / 0x41-43 read-only / scheduling wrap and malformed packets PASS");
    return 0;
}
