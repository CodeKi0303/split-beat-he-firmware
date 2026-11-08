#ifndef C86_ADC_ASYNC_H
#define C86_ADC_ASYNC_H
#include "platform.h"

#if SCAN_ADC_ASYNC
/* Main-only. A true result contains only this bank's global Hall indices:
 * ADC1=0..3, ADC2=4..6. Sequence/time belong to that bank, not both banks.
 * Repeated cursors return false; digital/encoder acquisition belongs to0. */
bool adc_scan_bank_snapshot(unsigned bank,c77_sample_t *out);
#if !ROLE_LEFT
/* Main-only. Prepare the next S7 snapshot; the link ISR only reads it. */
void adc_scan_satellite_service(void);
#endif
#endif
#endif
