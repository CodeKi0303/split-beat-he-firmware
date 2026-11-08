#include "adc_irq_probe.h"

#if defined(C86_ADC_IRQ_PROBE)
#include "adc_scan_private.h"
#include "keyboard.h"
#include "update_link.h"
#include "usb_device.h"
#include <string.h>
#ifndef ADC_IRQ_PROBE_HOST_TEST
#include "board_internal.h"
_Static_assert(C77_CPU_HZ==240000000U && SCAN_ADC_CAPTURE_DIAG &&
               SCAN_TARGET_HZ==64000U && !SCAN_DUAL_12BIT_BATCHED && !SCAN_DUAL_96K,
               "ADC IRQ probe is C86 matched64 diagnostic only");
#endif

#define PROBE_CPU_HZ 240000000U
#define PROBE_WINDOW (PROBE_CPU_HZ/2U)
#define PROBE_ACK_LIMIT (PROBE_CPU_HZ/20U)
enum { PROBE_EMPTY, PROBE_ARMED, PROBE_RUNNING, PROBE_FINISHED };
typedef struct { uint32_t total,max; } probe_row;
static probe_row rows[6];
static uint32_t state,run_id,armed_at,started_at,elapsed_cycles;
static uint32_t kept,discarded,rng,seq_start,seq_delta;
static usb_keyboard_stats_t usb_start,usb_end;
volatile uint32_t adc_irq_probe_active,adc_irq_probe_next;
uint32_t adc_irq_probe_seq,adc_irq_probe_mask,adc_irq_probe_points[4];

static void schedule(uint32_t sequence)
{
    /* A fixed stride that divides evenly by 8 would always measure the
     * same phase of the 8 kHz link. Choose 96..159 scans between samples. */
    rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
    adc_irq_probe_next=sequence+95U+(rng&63U);
}
void adc_irq_probe_finish(uint32_t entered,uint32_t published_sequence,uint32_t finished)
{
    schedule(published_sequence);
    if(adc_irq_probe_mask!=15U || published_sequence-adc_irq_probe_seq!=1U){
        discarded++;return;
    }
    uint32_t spans[6]={finished-entered};
    uint32_t previous=0U;
    for(unsigned i=0;i<4U;i++){
        const uint32_t offset=adc_irq_probe_points[i]-entered;
        if(offset<previous || offset>spans[0]){discarded++;return;}
        spans[i+1U]=offset-previous;previous=offset;
    }
    spans[5]=spans[0]-previous;
    for(unsigned i=0;i<6U;i++){
        rows[i].total+=spans[i];
        if(spans[i]>rows[i].max)rows[i].max=spans[i];
    }
    kept++;
}
bool adc_irq_probe_busy(void)
{
    return state==PROBE_ARMED || state==PROBE_RUNNING;
}
void adc_irq_probe_cancel(void)
{
    adc_irq_probe_active=0U;state=PROBE_EMPTY;
}
void adc_irq_probe_poll(bool vendor_drained)
{
    if(state==PROBE_ARMED){
        if(ADC_PROBE_CLOCK()-armed_at>=PROBE_ACK_LIMIT){adc_irq_probe_cancel();return;}
        if(!vendor_drained||!usb_vendor_idle())return;
        split_adc_snapshot adc;adc_scan_stats(&adc);
        usb_keyboard_stats(&usb_start);
        seq_start=adc.sample_seq;
        memset(rows,0,sizeof rows);kept=discarded=0U;
        rng=0x43fe670bU^run_id;if(!rng)rng=1U;
        schedule(seq_start);
        started_at=ADC_PROBE_CLOCK();
        state=PROBE_RUNNING;adc_irq_probe_active=1U;
    }else if(state==PROBE_RUNNING && ADC_PROBE_CLOCK()-started_at>=PROBE_WINDOW){
        adc_irq_probe_active=0U;
        elapsed_cycles=ADC_PROBE_CLOCK()-started_at;
        split_adc_snapshot adc;adc_scan_stats(&adc);seq_delta=adc.sample_seq-seq_start;
        usb_keyboard_stats(&usb_end);state=PROBE_FINISHED;
    }
}
static void put32(uint8_t *p,uint32_t v)
{
    for(unsigned i=0;i<4U;i++)p[i]=(uint8_t)(v>>(8U*i));
}
bool adc_irq_probe_command(const uint8_t q[64],uint8_t r[64],bool allow_start)
{
    if(q[0]!='C'||q[1]!='7'||q[3]!=ADC_IRQ_PROBE_COMMAND)return false;
    memset(r,0,64);r[0]='C';r[1]='7';r[2]=1;r[3]=ADC_IRQ_PROBE_COMMAND|0x80U;
    r[4]=q[4];r[5]=q[5];
    if(!update_frame_valid(q)||q[6]||q[7]<1U||q[8]>1U||q[7]!=q[8]+1U)r[6]=C77_BAD_PACKET;
    else if(q[8]==0U){
        if(!allow_start||adc_irq_probe_busy())r[6]=C77_BUSY;
        else {
            if(++run_id==0U)++run_id;
            armed_at=ADC_PROBE_CLOCK();state=PROBE_ARMED;
            put32(r+8,run_id);r[7]=4U;
        }
    }else if(q[9]>2U)r[6]=C77_BAD_INDEX;
    else if(adc_irq_probe_busy())r[6]=C77_BUSY;
    else if(state!=PROBE_FINISHED)r[6]=C77_NOT_READY;
    else {
        uint8_t *out=r+8;put32(out,run_id);
        if(q[9]==0U){
            const uint32_t fields[]={elapsed_cycles,PROBE_CPU_HZ,PROBE_WINDOW,kept,discarded,
                usb_end.published-usb_start.published,usb_end.completed-usb_start.completed,seq_delta,0U};
            for(unsigned i=0;i<9U;i++)put32(out+4U+i*4U,fields[i]);
            r[7]=40U;
        }else{
            out[4]=3U;
            for(unsigned i=0;i<3U;i++){
                const unsigned id=(q[9]-1U)*3U+i;
                uint8_t *record=out+5U+i*13U;
                record[0]=(uint8_t)id;put32(record+1U,kept);
                put32(record+5U,rows[id].total);put32(record+9U,rows[id].max);
            }
            r[7]=44U;
        }
    }
    update_frame_finish(r);return true;
}
#endif
