#ifndef KEYBOARD_SCAN_PROFILE_H
#define KEYBOARD_SCAN_PROFILE_H
#include "hardware_profile.h"

/* Acquisition rate is one complete seven-key frame per trigger. Explicit
 * C86-left dual64/96k candidates split4/3 ranks across ADC1/2.
 * Split link and USB stay8kHz, independent of the acquisition rate. */
#if defined(C86_SCAN_TARGET_HZ) && C86_SCAN_TARGET_HZ == 75000U && \
    (HARDWARE_REVISION != 86 || !defined(ROLE_LEFT) || ROLE_LEFT != 1)
#error The75k single-ADC candidate requires explicit C86 left hardware
#endif
#if HARDWARE_REVISION == 86
#ifndef C86_SCAN_TARGET_HZ
#define C86_SCAN_TARGET_HZ 64000U
#endif
#if C86_SCAN_TARGET_HZ != 16000U && C86_SCAN_TARGET_HZ != 64000U && C86_SCAN_TARGET_HZ != 75000U && C86_SCAN_TARGET_HZ != 96000U
#error C86 scan target must be16000,64000 or explicit left-only75000/96000Hz
#endif
#define SCAN_TARGET_HZ C86_SCAN_TARGET_HZ
#else
#define SCAN_TARGET_HZ 16000U
#endif

#ifndef C86_ADC_DUAL64
#define C86_ADC_DUAL64 0
#endif
#ifndef C86_ADC_ASYNC64
#define C86_ADC_ASYNC64 0
#endif
#ifndef C86_ADC_ASYNC96
#define C86_ADC_ASYNC96 0
#endif
#ifndef C86_ADC_CAPTURE_DIAG
#define C86_ADC_CAPTURE_DIAG 0
#endif
#if C86_ADC_CAPTURE_DIAG != 0 && C86_ADC_CAPTURE_DIAG != 1
#error C86_ADC_CAPTURE_DIAG must be0 or1
#endif
#define SCAN_ADC_CAPTURE_DIAG C86_ADC_CAPTURE_DIAG
#ifndef C86_ADC_DUAL64_MATCHED40
#define C86_ADC_DUAL64_MATCHED40 0
#endif
#if C86_ADC_DUAL64_MATCHED40 != 0 && C86_ADC_DUAL64_MATCHED40 != 1
#error C86_ADC_DUAL64_MATCHED40 must be0 or1
#endif
#if C86_ADC_DUAL64_MATCHED40 && (!C86_ADC_DUAL64 || !C86_ADC_CAPTURE_DIAG || HARDWARE_REVISION != 86 || !defined(ROLE_LEFT) || ROLE_LEFT != 1 || SCAN_TARGET_HZ != 64000U)
#error Matched40 dual64 ADC requires explicit C86 left dual64 diagnostic hardware at64000Hz
#endif
#ifndef C86_ADC_DUAL96_12BIT
#define C86_ADC_DUAL96_12BIT 0
#endif
#if C86_ADC_DUAL96_12BIT != 0 && C86_ADC_DUAL96_12BIT != 1
#error C86_ADC_DUAL96_12BIT must be0 or1
#endif
#if C86_ADC_DUAL96_12BIT && (HARDWARE_REVISION != 86 || !defined(ROLE_LEFT) || ROLE_LEFT != 1 || SCAN_TARGET_HZ != 96000U || C86_ADC_DUAL64)
#error Batched dual96 12-bit ADC requires explicit C86 left hardware at96000Hz without dual64
#endif
#define SCAN_DUAL_12BIT_BATCHED C86_ADC_DUAL96_12BIT
#if C86_ADC_DUAL64 != 0 && C86_ADC_DUAL64 != 1
#error C86_ADC_DUAL64 must be0 or1
#endif
#if C86_ADC_DUAL64 && (HARDWARE_REVISION != 86 || !defined(ROLE_LEFT) || ROLE_LEFT != 1 || SCAN_TARGET_HZ != 64000U)
#error Dual64 ADC requires explicit C86 left hardware at64000Hz
#endif
#if C86_ADC_CAPTURE_DIAG && (HARDWARE_REVISION != 86 || !defined(ROLE_LEFT) || ROLE_LEFT != 1 || SCAN_TARGET_HZ != 64000U || C86_ADC_DUAL96_12BIT || (C86_ADC_DUAL64 && !C86_ADC_DUAL64_MATCHED40))
#error ADC capture diagnostic requires C86 left64000Hz single ADC or matched40 dual64
#endif
#if C86_ADC_ASYNC64 != 0 && C86_ADC_ASYNC64 != 1
#error C86_ADC_ASYNC64 must be0 or1
#endif
#if C86_ADC_ASYNC64 && (!C86_ADC_DUAL64 || C86_ADC_CAPTURE_DIAG || C86_ADC_DUAL64_MATCHED40 || C86_ADC_DUAL96_12BIT || HARDWARE_REVISION != 86 || !defined(ROLE_LEFT) || ROLE_LEFT != 1 || SCAN_TARGET_HZ != 64000U)
#error Async DMA requires explicit C86 left64k and its independent dual ADC profile
#endif
#if C86_ADC_ASYNC96 != 0 && C86_ADC_ASYNC96 != 1
#error C86_ADC_ASYNC96 must be0 or1
#endif
#if C86_ADC_ASYNC96 && (C86_ADC_ASYNC64 || C86_ADC_DUAL64 || C86_ADC_CAPTURE_DIAG || C86_ADC_DUAL64_MATCHED40 || C86_ADC_DUAL96_12BIT || HARDWARE_REVISION != 86 || !defined(ROLE_LEFT) || (ROLE_LEFT != 0 && ROLE_LEFT != 1) || SCAN_TARGET_HZ != 96000U)
#error Async DMA96 requires an explicit C86 role at96k with no competing ADC profile
#endif
#define SCAN_ADC_ASYNC (C86_ADC_ASYNC64 || C86_ADC_ASYNC96)

#define SCAN_ADC_RANKS 7U
#define SCAN_TIMER_HZ 120000000U
#define SCAN_TIMER_PERIOD (SCAN_TIMER_HZ / SCAN_TARGET_HZ - 1U)
#if C86_ADC_DUAL64 || C86_ADC_DUAL96_12BIT || SCAN_ADC_ASYNC
#define SCAN_DUAL_64K C86_ADC_DUAL64
#if SCAN_ADC_ASYNC
#define SCAN_ADC_DMA_FRAMES_PER_HALF 16U
#elif C86_ADC_DUAL64
#define SCAN_ADC_DMA_FRAMES_PER_HALF 1U
#else
#define SCAN_ADC_DMA_FRAMES_PER_HALF 2U
#endif
/* SCAN_DUAL_96K gates the existing14-bit Q5 pipeline: never enable it here. */
#define SCAN_DUAL_96K 0
#define SCAN_ADC1_RANKS 4U
#define SCAN_ADC2_RANKS 3U
#if SCAN_ADC_ASYNC
#define SCAN_PUBLICATION_HZ 0U /* Foreground consumes the latest complete window. */
#else
#define SCAN_PUBLICATION_HZ (SCAN_TARGET_HZ/SCAN_ADC_DMA_FRAMES_PER_HALF)
#endif
#define SCAN_ADC_BITS 12U
#define SCAN_ADC_SAMPLING_CYCLES_X2 129U /* 64.5 cycles in both comparison profiles. */
#define SCAN_ADC_CONVERSION_CYCLES_X2 13U /* 12-bit conversion6.5 cycles. */
#define SCAN_ADC_USE_PLL2 1
#if C86_ADC_DUAL64_MATCHED40 || SCAN_ADC_ASYNC
/* Explicit experiment: match the installed single-ADC clock/sample duration.
 * This is not a higher-accuracy default:40MHz is above AN5354's typical
 * LQFP100 dual-ADC12-bit35MHz no-missing-code characterization condition. */
#define SCAN_ADC_PLL2_P_DIV 6U /* 480MHz VCO /6 ->80MHz, fixed /2 ->40MHz. */
#define SCAN_ADC_KERNEL_HZ 80000000U
#else
#define SCAN_ADC_PLL2_P_DIV 8U /* 480MHz VCO /8 ->60MHz, fixed /2 ->30MHz. */
#define SCAN_ADC_KERNEL_HZ 60000000U
#endif
#define SCAN_ADC_IRQ_PRIORITY 1U
#elif SCAN_TARGET_HZ == 96000U
#if !defined(ROLE_LEFT) || ROLE_LEFT != 1
#error The96k dual-ADC candidate is only supported on the explicitly selected C86 left board
#endif
#define SCAN_DUAL_96K 1
#define SCAN_DUAL_64K 0
#define SCAN_ADC1_RANKS 4U
#define SCAN_ADC2_RANKS 3U
#define SCAN_ADC_DMA_FRAMES_PER_HALF 2U
#define SCAN_PUBLICATION_HZ (SCAN_TARGET_HZ/SCAN_ADC_DMA_FRAMES_PER_HALF)
#define SCAN_ADC_BITS 14U
#define SCAN_ADC_SAMPLING_CYCLES_X2 65U /* 32.5 cycles at20MHz =1.625us. */
#define SCAN_ADC_CONVERSION_CYCLES_X2 15U /* 14-bit conversion7.5 cycles. */
#define SCAN_ADC_USE_PLL2 1
#define SCAN_ADC_PLL2_P_DIV 12U /* 480MHz VCO /12 ->40MHz, fixed /2 ->20MHz. */
#define SCAN_ADC_KERNEL_HZ 40000000U
#define SCAN_ADC_IRQ_PRIORITY 1U
#else
#define SCAN_DUAL_64K 0
#define SCAN_DUAL_96K 0
#define SCAN_ADC1_RANKS 7U
#define SCAN_ADC2_RANKS 0U
#define SCAN_ADC_BITS 12U
#define SCAN_ADC_SAMPLING_CYCLES_X2 129U /* 64.5 cycles, unchanged. */
#define SCAN_ADC_CONVERSION_CYCLES_X2 13U /* ADC1 at 12-bit resolution. */
#if SCAN_TARGET_HZ == 64000U || SCAN_TARGET_HZ == 75000U
#define SCAN_ADC_USE_PLL2 1
#define SCAN_ADC_PLL2_P_DIV 6U /* 480MHz VCO /6, UART Q /4 remains 120MHz. */
#define SCAN_ADC_KERNEL_HZ 80000000U
#define SCAN_ADC_IRQ_PRIORITY 1U
#else
#define SCAN_ADC_USE_PLL2 0
#define SCAN_ADC_PLL2_P_DIV 4U
#define SCAN_ADC_KERNEL_HZ 25000000U
#define SCAN_ADC_IRQ_PRIORITY 2U
#endif
#endif
/* RM0468 ADC1/2 fixed /2 follows the asynchronous /1 prescaler on H723. */
#define SCAN_ADC_EFFECTIVE_HZ (SCAN_ADC_KERNEL_HZ / 2U)
#define SCAN_ADC_SEQUENCE_CYCLES_X2 \
    (SCAN_ADC1_RANKS * (SCAN_ADC_SAMPLING_CYCLES_X2 + SCAN_ADC_CONVERSION_CYCLES_X2))

_Static_assert(SCAN_TIMER_HZ % SCAN_TARGET_HZ == 0U, "Scan timer must divide exactly");
_Static_assert(SCAN_ADC_EFFECTIVE_HZ <= 50000000U, "H723 ADC clock exceeds DS13313 limit");
/* Include the documented worst asynchronous trigger latency (3.5 ADC cycles).
 * This checks conversion capacity, not external sensor settling or ISR load. */
_Static_assert((SCAN_ADC_SEQUENCE_CYCLES_X2 + 7ULL) * SCAN_TARGET_HZ <
               2ULL * SCAN_ADC_EFFECTIVE_HZ, "ADC sequence exceeds trigger interval");
#endif
