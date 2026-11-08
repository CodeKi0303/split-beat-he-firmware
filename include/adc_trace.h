#ifndef C86_ADC_TRACE_H
#define C86_ADC_TRACE_H
#include "platform.h"

#if SCAN_ADC_CAPTURE_DIAG
enum { ADC_TRACE_KEYS=2, ADC_TRACE_HISTORY=8, ADC_TRACE_COMMAND=0x44 };
#define ADC_TRACE_CAPABILITY 0x400U
typedef struct { uint16_t raw[ADC_TRACE_KEYS][ADC_TRACE_HISTORY]; } adc_trace_history;

/* Copy the publication and matching rolling history with the same ADC IRQ
 * mask. DMA can continue; neither the published slot nor history is DMA RAM. */
bool adc_scan_trace_snapshot(c77_sample_t *out,adc_trace_history *history,
                             const uint8_t keys[ADC_TRACE_KEYS]);
#endif
#endif
