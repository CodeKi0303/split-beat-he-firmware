#include "cpu_profile.h"

#if defined(C86_CPU_PROFILE)
#include "keyboard.h"
#include "update_link.h"
#include "usb_device.h"
#include <string.h>
#ifndef CPU_PROFILE_HOST_TEST
#include "board_internal.h"
_Static_assert(C77_CPU_HZ==240000000U && SCAN_ADC_CAPTURE_DIAG,"CPU profiling is C86 diagnostic only");
#endif

#define CPU_PROF_HZ 240000000U
#define CPU_PROF_WINDOW (CPU_PROF_HZ/4U)
#define CPU_PROF_ARM_LIMIT (CPU_PROF_HZ/20U)
enum { PROF_EMPTY, PROF_ARMED, PROF_RUNNING, PROF_FINISHED };
typedef struct { uint32_t calls,total,max; } cpu_prof_row;
static cpu_prof_row stages[CPU_PROF_COUNT];
static uint32_t state,run_id,armed_at,started_at,elapsed_cycles;
static usb_keyboard_stats_t usb_start,usb_stop;
volatile uint32_t cpu_prof_active,cpu_prof_irq_cycles;
uint32_t cpu_prof_rt_calls;

static void add_irq_cycles(uint32_t cycles)
{
#ifdef CPU_PROFILE_HOST_TEST
    cpu_prof_irq_cycles+=cycles;
#else
    /* Higher-priority IRQs can complete while a lower one records its result. */
    (void)__atomic_fetch_add(&cpu_prof_irq_cycles,cycles,__ATOMIC_RELAXED);
#endif
}
static void add_irq_row(cpu_prof_row *row,uint32_t cycles)
{
#ifdef CPU_PROFILE_HOST_TEST
    row->calls++;row->total+=cycles;if(cycles>row->max)row->max=cycles;
#else
    (void)__atomic_fetch_add(&row->calls,1U,__ATOMIC_RELAXED);
    (void)__atomic_fetch_add(&row->total,cycles,__ATOMIC_RELAXED);
    uint32_t previous=__atomic_load_n(&row->max,__ATOMIC_RELAXED);
    while(cycles>previous &&
          !__atomic_compare_exchange_n(&row->max,&previous,cycles,true,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
#endif
}

void cpu_prof_end(cpu_prof_stamp start,unsigned stage,bool irq)
{
    const cpu_prof_stamp end=cpu_prof_mark();
    if(!end.active)return;
    const uint32_t span=end.cycles-start.cycles,nested=end.irq_cycles-start.irq_cycles;
    const uint32_t cycles=span>=nested?span-nested:0U;
    cpu_prof_row *row=&stages[stage];
    /* ADC handlers share priority 1 and split handlers priority 2. The
     * system bucket is the only one shared by different priorities. */
    if(irq && stage==CPU_PROF_IRQ_SYSTEM)add_irq_row(row,cycles);
    else {row->calls++;row->total+=cycles;if(cycles>row->max)row->max=cycles;}
    if(irq)add_irq_cycles(cycles);
}
bool cpu_profile_busy(void)
{
    return state==PROF_ARMED || state==PROF_RUNNING;
}
void cpu_profile_cancel(void)
{
    cpu_prof_active=0U;state=PROF_EMPTY;
}
void cpu_profile_poll(bool vendor_drained)
{
    if(state==PROF_ARMED){
        if(CPU_PROF_CLOCK()-armed_at>=CPU_PROF_ARM_LIMIT){cpu_profile_cancel();return;}
        if(!vendor_drained||!usb_vendor_idle())return;
        usb_keyboard_stats(&usb_start);
        memset(stages,0,sizeof stages);
        cpu_prof_irq_cycles=0U;cpu_prof_rt_calls=0U;
        started_at=CPU_PROF_CLOCK();
        state=PROF_RUNNING;cpu_prof_active=1U;
    } else if(state==PROF_RUNNING && CPU_PROF_CLOCK()-started_at>=CPU_PROF_WINDOW){
        cpu_prof_active=0U;
        elapsed_cycles=CPU_PROF_CLOCK()-started_at;
        usb_keyboard_stats(&usb_stop);
        state=PROF_FINISHED;
    }
}

static void put32(uint8_t *p,uint32_t v)
{
    for(unsigned i=0;i<4U;i++)p[i]=(uint8_t)(v>>(8U*i));
}
bool cpu_profile_command(const uint8_t q[64],uint8_t r[64],bool allow_start)
{
    if(q[0]!='C'||q[1]!='7'||q[3]!=CPU_PROFILE_COMMAND)return false;
    memset(r,0,64);r[0]='C';r[1]='7';r[2]=1;r[3]=CPU_PROFILE_COMMAND|0x80U;
    r[4]=q[4];r[5]=q[5];
    if(!update_frame_valid(q)||q[6]||q[7]<1U||q[8]>1U||q[7]!=q[8]+1U)r[6]=C77_BAD_PACKET;
    else if(q[8]==0U){
        if(!allow_start||cpu_profile_busy())r[6]=C77_BUSY;
        else {
            if(++run_id==0U)++run_id;
            armed_at=CPU_PROF_CLOCK();state=PROF_ARMED;
            put32(r+8,run_id);r[7]=4U;
        }
    }else if(q[9]>4U)r[6]=C77_BAD_INDEX;
    else if(cpu_profile_busy())r[6]=C77_BUSY;
    else if(state!=PROF_FINISHED)r[6]=C77_NOT_READY;
    else {
        uint8_t *out=r+8;put32(out,run_id);
        if(q[9]==0U){
            const uint32_t fields[]={elapsed_cycles,CPU_PROF_HZ,CPU_PROF_WINDOW,
                usb_stop.published-usb_start.published,usb_stop.completed-usb_start.completed,
                cpu_prof_irq_cycles,stages[CPU_PROF_SERVICE].calls};
            for(unsigned i=0;i<7U;i++)put32(out+4U+i*4U,fields[i]);
            r[7]=32U;
        }else{
            out[4]=3U;
            for(unsigned i=0;i<3U;i++){
                const unsigned id=(q[9]-1U)*3U+i;
                uint8_t *record=out+5U+i*13U;
                record[0]=(uint8_t)id;
                put32(record+1U,id==CPU_PROF_RT_CALLS?cpu_prof_rt_calls:stages[id].calls);
                put32(record+5U,stages[id].total);put32(record+9U,stages[id].max);
            }
            r[7]=44U;
        }
    }
    update_frame_finish(r);return true;
}
#endif
