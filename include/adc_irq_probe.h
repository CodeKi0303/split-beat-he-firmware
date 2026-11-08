#ifndef C86_ADC_IRQ_PROBE_H
#define C86_ADC_IRQ_PROBE_H
#include <stdbool.h>
#include <stdint.h>

#if defined(C86_ADC_IRQ_PROBE)
#ifdef ADC_IRQ_PROBE_HOST_TEST
extern volatile uint32_t adc_irq_probe_test_cycles;
#define ADC_PROBE_CLOCK() (adc_irq_probe_test_cycles)
#else
#include "stm32h7xx.h"
#define ADC_PROBE_CLOCK() (DWT->CYCCNT)
#endif

#define ADC_IRQ_PROBE_COMMAND 0x46U
#define ADC_IRQ_PROBE_CAPABILITY 0x1000U
extern volatile uint32_t adc_irq_probe_active,adc_irq_probe_next;
extern uint32_t adc_irq_probe_seq,adc_irq_probe_mask,adc_irq_probe_points[4];

/* One flag read when idle; one sequence check when armed. Only Stream0
 * updates selected samples. The unrelated higher-rate IRQs remain untouched. */
static inline bool adc_irq_probe_select(uint32_t sequence)
{
    if(!adc_irq_probe_active || sequence-adc_irq_probe_next>=0x80000000U)return false;
    adc_irq_probe_seq=sequence;adc_irq_probe_mask=0U;
    return true;
}
static inline void adc_irq_probe_mark(unsigned point)
{
    adc_irq_probe_points[point-1U]=ADC_PROBE_CLOCK();
    adc_irq_probe_mask|=1U<<(point-1U);
}
void adc_irq_probe_finish(uint32_t entered,uint32_t published_sequence,uint32_t finished);
void adc_irq_probe_poll(bool reply_queued);
void adc_irq_probe_cancel(void);
bool adc_irq_probe_busy(void);
bool adc_irq_probe_command(const uint8_t request[64],uint8_t reply[64],bool allow_start);
#endif
#endif
