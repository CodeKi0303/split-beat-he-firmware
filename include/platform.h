#ifndef C77_PLATFORM_H
#define C77_PLATFORM_H
#include <stdint.h>
#include <stdbool.h>
#include "scan_profile.h"
typedef struct {
    uint16_t raw[7];
    /* Internal local-acquisition data only: not a field in the S7 wire packet.
     * raw remains the unfiltered sample for diagnostics and calibration. */
    uint16_t hall_filtered[7];
#if SCAN_DUAL_96K
    /* 14-bit, eight-scan SUM in legacy 12-bit ADC units with five fractional
     * bits. This preserves the average without an integer ADC-code rounding.
     * Private left acquisition only; never serialized by the split codec. */
    uint32_t hall_q5[7];
#endif
    uint16_t encoders[2];
    uint8_t digital,valid,hall_filtered_valid;
#if SCAN_DUAL_96K
    uint8_t hall_q5_valid;
#endif
    uint32_t sequence,timestamp_us,errors;
} c77_sample_t;
/* Keep the common sample small enough for inline copies in the scan ISR. */
_Static_assert(sizeof(c77_sample_t)==(SCAN_DUAL_96K ? 76U : 48U),
               "Unexpected private acquisition sample size");
void board_init(void);
void board_start(void);
void board_stop(void);
void board_update_start(void);
void board_update_pause(void);
void board_update_resume(void);
void board_update_power_off(void);
uint32_t board_micros(void);
bool board_local_snapshot(c77_sample_t *out);
bool board_remote_snapshot(c77_sample_t *out);
void board_service(void);
uint16_t board_flash_kib(void);
void board_reboot(void);
void Error_Handler(void);
#endif
