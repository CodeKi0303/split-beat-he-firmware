#ifndef C77_HALL_HISTORY_H
#define C77_HALL_HISTORY_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "hall_filter.h"

/* Acquisition stores every scan; foreground sums a coherent copied history.
 * No decimation enters the average. Eight14-bit samples sum exactly to the
 * eight-sample mean in12-bit-equivalent Q5 units. */
typedef struct {
    uint16_t samples[HALL_FILTER_MAX_SAMPLES][HALL_FILTER_CHANNELS];
    unsigned next,ready;
} hall_history_t;

static inline void hall_history_reset(hall_history_t *history)
{
    memset(history,0,sizeof *history);
}

/* Caller validates all incoming raw values before committing a batch. */
static inline bool hall_history_push2(hall_history_t *history,
                                     const uint16_t raw[2][HALL_FILTER_CHANNELS])
{
    if(history->next>=HALL_FILTER_MAX_SAMPLES || history->ready>1U) return false;
    unsigned first=0;
    if(!history->ready) {
        for(unsigned slot=0;slot<HALL_FILTER_MAX_SAMPLES;slot++)
            for(unsigned key=0;key<HALL_FILTER_CHANNELS;key++)history->samples[slot][key]=raw[0][key];
        history->ready=1U;history->next=0U;first=1U;
    }
    for(unsigned frame=first;frame<2U;frame++) {
        for(unsigned key=0;key<HALL_FILTER_CHANNELS;key++)history->samples[history->next][key]=raw[frame][key];
        history->next=(history->next+1U)%HALL_FILTER_MAX_SAMPLES;
    }
    return true;
}

static inline bool hall_history_q5(const hall_history_t *history,uint32_t out_q5[HALL_FILTER_CHANNELS])
{
    if(history->ready!=1U || history->next>=HALL_FILTER_MAX_SAMPLES) return false;
    for(unsigned key=0;key<HALL_FILTER_CHANNELS;key++) {
        uint32_t sum=0;
        for(unsigned slot=0;slot<HALL_FILTER_MAX_SAMPLES;slot++) {
            const uint16_t value=history->samples[slot][key];
            if(value>HALL_FILTER_ADC14_MAX)return false;
            sum+=value;
        }
        out_q5[key]=sum;
    }
    return true;
}
#endif
