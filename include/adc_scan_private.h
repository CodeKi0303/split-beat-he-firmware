#ifndef SPLIT_ADC_SCAN_H
#define SPLIT_ADC_SCAN_H

#include <stdint.h>
#include "scan_profile.h"

#define SPLIT_ADC_HZ SCAN_TARGET_HZ
#define SPLIT_ADC_RANKS SCAN_ADC_RANKS

typedef struct {
    uint32_t raw0,raw1;
    uint32_t sample_seq,sample_cycles;
    uint32_t running,errors,overruns,dma_errors,late_irqs,init_error;
    uint32_t period_min_cycles,period_max_cycles;
    uint16_t raw[SPLIT_ADC_RANKS];
    uint32_t irq_max_cycles;
    /* First runtime stop, captured before disabling DMA. Phase is stop-check
     * cycles since expected TIM3 trigger (last scan for a batched profile);
     * check time is IRQ entry or the post-flag epoch check. Wrap is intentional. */
    uint32_t stop_reason,stop_phase_cycles,stop_dma_flags,stop_ndtr1,stop_ndtr2;
} split_adc_snapshot;

enum {
    SPLIT_ADC_STOP_NONE=0,
    SPLIT_ADC_STOP_FLAGS=1,
    SPLIT_ADC_STOP_EPOCH=2,
    SPLIT_ADC_STOP_OWNERSHIP=3,
    SPLIT_ADC_STOP_RAW_RANGE=4,
    SPLIT_ADC_STOP_FILTER=5,
    SPLIT_ADC_STOP_OVR=6,
    SPLIT_ADC_STOP_DMA=7
};

/* Main-context coherent diagnostic read. Sequence deltas over host elapsed
 * time measure acquisition rate; period bounds include interrupt scheduling. */
void adc_scan_stats(split_adc_snapshot *out);

enum {
    SPLIT_ADC_INIT_OK=0,
    SPLIT_ADC_INIT_CLOCK=1,
    SPLIT_ADC_INIT_MEMORY=2,
    SPLIT_ADC_INIT_CYCLE_COUNTER=3,
    SPLIT_ADC_INIT_DMA=4,
    SPLIT_ADC_INIT_TIMER=5,
    SPLIT_ADC_INIT_ADC=6,
    SPLIT_ADC_INIT_CHANNEL=7,
    SPLIT_ADC_INIT_CALIBRATION=8,
    SPLIT_ADC_INIT_DMA_START=9,
    SPLIT_ADC_INIT_TIMER_START=10
};


#endif
