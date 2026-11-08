#ifndef C77_BOARD_INTERNAL_H
#define C77_BOARD_INTERNAL_H
#include "platform.h"
#include "hardware_profile.h"
#include "scan_profile.h"
#include "stm32h7xx_hal.h"
#ifndef ROLE_LEFT
#error ROLE_LEFT must be defined as 0 or 1
#endif
#define C77_CPU_HZ 240000000U
#define C77_SCAN_HZ SCAN_TARGET_HZ
#define C77_SCAN_RANKS 7U
bool adc_scan_start(void);
void adc_scan_stop(void);
bool adc_scan_snapshot(c77_sample_t *out);
void split_link_start(void);
void split_link_stop(void);
void split_link_allow(bool allowed);
bool split_link_snapshot(c77_sample_t *out);
void board_input_scan(c77_sample_t *sample);
#if HARDWARE_CC_FUSB303B && ROLE_LEFT
void board_cc_init(void);
void board_cc_service(void);
void board_cc_stop(void);
#endif
#endif
