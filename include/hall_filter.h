#ifndef C77_HALL_FILTER_H
#define C77_HALL_FILTER_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define HALL_FILTER_CHANNELS 7U
#define HALL_FILTER_MAX_SAMPLES 8U
#define HALL_FILTER_ADC14_MAX 16383U
#define HALL_FILTER_Q5_MAX (HALL_FILTER_ADC14_MAX * HALL_FILTER_MAX_SAMPLES)

/* Fixed work per ADC scan, no allocation and no floating point. The first
 * sample seeds every slot, so startup does not ramp up from artificial zero.
 * A rounded ADC-count average is not a claim of sub-ADC physical resolution. */
typedef struct {
    uint16_t history[HALL_FILTER_MAX_SAMPLES][HALL_FILTER_CHANNELS];
    uint32_t sum[HALL_FILTER_CHANNELS];
    unsigned next,ready;
} hall_filter_t;

static inline void hall_filter_reset(hall_filter_t *filter)
{
    memset(filter,0,sizeof *filter);
}

static inline bool hall_filter_push(hall_filter_t *filter,
                                   const uint16_t raw[HALL_FILTER_CHANNELS],
                                   uint16_t filtered[HALL_FILTER_CHANNELS],
                                   unsigned window)
{
    if(window==0U || window>HALL_FILTER_MAX_SAMPLES) return false;
    /* Never average an invalid ADC into a plausible normal value. Retain the
     * original raw frame for the engine's existing validity checks. */
    for(unsigned channel=0U;channel<HALL_FILTER_CHANNELS;channel++) {
        if(raw[channel]>4095U) {
            hall_filter_reset(filter);
            return false;
        }
    }
    if(!filter->ready) {
        for(unsigned channel=0U;channel<HALL_FILTER_CHANNELS;channel++) {
            for(unsigned slot=0U;slot<window;slot++)
                filter->history[slot][channel]=raw[channel];
            filter->sum[channel]=(uint32_t)raw[channel]*window;
            filtered[channel]=raw[channel];
        }
        filter->next=0U;filter->ready=1U;
        return true;
    }
    const unsigned slot=filter->next;
    for(unsigned channel=0U;channel<HALL_FILTER_CHANNELS;channel++) {
        filter->sum[channel]-=filter->history[slot][channel];
        filter->sum[channel]+=raw[channel];
        filter->history[slot][channel]=raw[channel];
        filtered[channel]=(uint16_t)((filter->sum[channel]+window/2U)/window);
    }
    filter->next=(slot+1U)%window;
    return true;
}

/* Eight14-bit samples sum to the average in12-bit-equivalent Q5 units:
 * sum/8/4 *32 == sum. No fractional information is rounded away here.
 * Reset the instance before changing acquisition mode. Raw14 input and
 * running state are checked before mutation; a bad frame cannot look valid
 * after averaging, and corrupted sums cannot wrap into a normal sample. */
static inline bool hall_filter_push14_q5(hall_filter_t *filter,
                                        const uint16_t raw14[HALL_FILTER_CHANNELS],
                                        uint32_t out_q5[HALL_FILTER_CHANNELS])
{
    if(filter==NULL || raw14==NULL || out_q5==NULL) return false;
    for(unsigned channel=0U;channel<HALL_FILTER_CHANNELS;channel++) {
        if(raw14[channel]>HALL_FILTER_ADC14_MAX) {
            hall_filter_reset(filter);
            return false;
        }
    }
    if(!filter->ready) {
        for(unsigned channel=0U;channel<HALL_FILTER_CHANNELS;channel++) {
            for(unsigned slot=0U;slot<HALL_FILTER_MAX_SAMPLES;slot++)
                filter->history[slot][channel]=raw14[channel];
            filter->sum[channel]=(uint32_t)raw14[channel]*HALL_FILTER_MAX_SAMPLES;
            out_q5[channel]=filter->sum[channel];
        }
        filter->next=0U;filter->ready=1U;
        return true;
    }
    if(filter->next>=HALL_FILTER_MAX_SAMPLES) {
        hall_filter_reset(filter);
        return false;
    }
    const unsigned slot=filter->next;
    for(unsigned channel=0U;channel<HALL_FILTER_CHANNELS;channel++) {
        const uint32_t old=filter->history[slot][channel],sum=filter->sum[channel];
        if(old>HALL_FILTER_ADC14_MAX || sum>HALL_FILTER_Q5_MAX || sum<old ||
           sum-old+raw14[channel]>HALL_FILTER_Q5_MAX) {
            hall_filter_reset(filter);
            return false;
        }
    }
    for(unsigned channel=0U;channel<HALL_FILTER_CHANNELS;channel++) {
        filter->sum[channel]-=filter->history[slot][channel];
        filter->sum[channel]+=raw14[channel];
        filter->history[slot][channel]=raw14[channel];
        out_q5[channel]=filter->sum[channel];
    }
    filter->next=(slot+1U)%HALL_FILTER_MAX_SAMPLES;
    return true;
}
#endif
