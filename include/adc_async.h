#ifndef C86_ADC_ASYNC_H
#define C86_ADC_ASYNC_H
#include "platform.h"

#if SCAN_ADC_ASYNC
/* Main-only. A true result contains only this bank's global Hall indices:
 * ADC1=0..3, ADC2=4..6. Sequence/time belong to that bank, not both banks.
 * Repeated cursors return false; digital/encoder acquisition belongs to0. */
bool adc_scan_bank_snapshot(unsigned bank,c77_sample_t *out);
#if SCAN_ADC_PHASE_PAIR
#define ADC_TUNING_COMMAND 0x47U
#define ADC_COMPARE_COMMAND 0x48U
#define ADC_PAIR_COMMAND 0x49U
uint32_t adc_scan_rate(void);
uint8_t adc_scan_filter(void);
bool adc_scan_parse_tuning(const uint8_t payload[5],uint32_t *hz,uint8_t *filter);
bool adc_scan_tune(uint32_t hz,uint8_t filter);
#endif
#if !ROLE_LEFT
/* Main-only. Prepare the next S7 snapshot; the link ISR only reads it. */
void adc_scan_satellite_service(void);
#endif
#endif
#endif
