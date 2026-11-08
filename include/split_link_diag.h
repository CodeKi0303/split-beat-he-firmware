#ifndef C86_SPLIT_LINK_DIAG_H
#define C86_SPLIT_LINK_DIAG_H
#include <stdint.h>

/* Cumulative modulo-2^32 since split_link_start; maintenance does not reset.
 * Left: polls=normal timer opportunities, accepted=new decoded sample replies.
 * Right: polls=decoded requests, accepted=normal replies submitted to UART.
 * CRC/framing are normal S7 parser counters, not updater C7 parser counters.
 * tx_fail also includes fatal RX/TX DMA transport errors; uart_errors counts
 * USART overrun/framing/noise/parity IRQs, including maintenance transport.
 * RTT includes expected decoded replies rejected by the 110us late limit;
 * RTT extrema are zero until the first such reply. */
typedef struct {
    uint32_t polls,accepted,timeouts,sequence_errors,late_replies,uart_errors;
    uint32_t tx_busy,tx_fail,crc_errors,framing_errors,rtt_min_us,rtt_max_us;
    /* Scheduling diagnostics: IRQ duration includes preemption. RX bytes are
     * consumed DMA bytes, including invalid/update frames, not valid packets.
     * flags: bit0 active, bit1 allowed, bit2 transmitting, bit3 update_mode. */
    uint32_t flags,rx_bytes,uart_irq_max_cycles,poll_irq_max_cycles,uart_irqs,poll_irqs;
} split_link_stats_t;
void split_link_stats(split_link_stats_t *out);

#endif
