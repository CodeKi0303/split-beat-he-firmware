#ifndef C86_CPU_PROFILE_H
#define C86_CPU_PROFILE_H
#include <stdbool.h>
#include <stdint.h>

#if defined(C86_CPU_PROFILE)
#ifdef CPU_PROFILE_HOST_TEST
extern volatile uint32_t cpu_prof_test_cycles;
#define CPU_PROF_CLOCK() (cpu_prof_test_cycles)
#else
#include "stm32h7xx.h"
#define CPU_PROF_CLOCK() (DWT->CYCCNT)
#endif

enum {
    CPU_PROF_SERVICE, CPU_PROF_LEFT, CPU_PROF_RIGHT, CPU_PROF_REPORT, CPU_PROF_VENDOR,
    CPU_PROF_IRQ_ADC, CPU_PROF_IRQ_SPLIT, CPU_PROF_IRQ_USB, CPU_PROF_IRQ_SYSTEM,
    CPU_PROF_CURVE_MISS, CPU_PROF_LINEAR_MISS, CPU_PROF_RT_CALLS, CPU_PROF_COUNT
};
#define CPU_PROFILE_COMMAND 0x45U
#define CPU_PROFILE_CAPABILITY 0x800U
typedef struct { uint32_t cycles, irq_cycles; bool active; } cpu_prof_stamp;
extern volatile uint32_t cpu_prof_active,cpu_prof_irq_cycles;
extern uint32_t cpu_prof_rt_calls;

static inline cpu_prof_stamp cpu_prof_mark(void)
{
    cpu_prof_stamp stamp={0,0,false};
    if(cpu_prof_active){
        uint32_t before;
        /* If a wrapped IRQ completed across the clock read, take a new pair.
         * An active higher-priority handler cannot run simultaneously here. */
        do {
            before=cpu_prof_irq_cycles;
            stamp.cycles=CPU_PROF_CLOCK();
            stamp.irq_cycles=cpu_prof_irq_cycles;
        } while(before!=stamp.irq_cycles);
        stamp.active=true;
    }
    return stamp;
}

void cpu_prof_end(cpu_prof_stamp start,unsigned stage,bool irq);
void cpu_profile_poll(bool vendor_drained);
void cpu_profile_cancel(void);
bool cpu_profile_busy(void);
bool cpu_profile_command(const uint8_t request[64],uint8_t reply[64],bool allow_start);

#define CPU_PROFILE_BEGIN(token) const cpu_prof_stamp token=cpu_prof_mark()
#define CPU_PROFILE_END(token, stage) do { if((token).active)cpu_prof_end((token),(stage),false); } while(0)
#define CPU_PROFILE_IRQ_END(token, stage) do { if((token).active)cpu_prof_end((token),(stage),true); } while(0)
#define CPU_PROFILE_RT_CALL() do { if(cpu_prof_active)++cpu_prof_rt_calls; } while(0)
#else
#define CPU_PROFILE_BEGIN(token) ((void)0)
#define CPU_PROFILE_END(token, stage) ((void)0)
#define CPU_PROFILE_IRQ_END(token, stage) ((void)0)
#define CPU_PROFILE_RT_CALL() ((void)0)
#endif
#endif
