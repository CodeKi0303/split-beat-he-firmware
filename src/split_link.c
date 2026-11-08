#include "board_internal.h"
#include "split_codec.h"
#include "split_link_diag.h"
#include "update_link.h"
#if defined(C86_CPU_PROFILE)
#include "cpu_profile.h"
#endif
#include <string.h>

#define RX_SIZE 128U
static volatile uint8_t rx_dma[RX_SIZE] __attribute__((section(".dma_buffer"),aligned(32)));
static uint8_t tx_dma[64] __attribute__((section(".dma_buffer"),aligned(32)));
static uint32_t tail;
static volatile bool active,allowed,transmitting;
static volatile uint32_t errors;
static volatile split_link_stats_t link_diagnostics;
static c77_split_parser_t parser;
static volatile c77_sample_t remote;
static volatile bool update_mode,update_pending;
static update_frame_parser update_parser;
static uint8_t update_packet[64];
static volatile uint32_t update_received_at;
static volatile uint32_t update_mode_at;
static volatile bool update_tx,update_tx_ok;
#if ROLE_LEFT
static bool awaiting,have_sequence;
static uint16_t next_sequence,expected;
static uint32_t sent_at,last_sequence;
#else
static bool reply_pending;
static uint16_t reply_sequence;
#endif

static void receive_mode(void)
{
    GPIOD->BSRR=(uint32_t)GPIO_PIN_14<<16;
    if(allowed) GPIOE->BSRR=(uint32_t)GPIO_PIN_3<<16;
    else GPIOE->BSRR=GPIO_PIN_3;
}
static void abort_tx(void)
{
    USART3->CR1&=~USART_CR1_TCIE;USART3->CR3&=~USART_CR3_DMAT;
    DMA1_Stream1->CR&=~DMA_SxCR_EN;
    transmitting=false;update_tx=false;update_tx_ok=false;
    receive_mode();
}
static bool start_tx(const uint8_t *p,unsigned count)
{
    if(!active||!allowed) {if(count!=64U)link_diagnostics.tx_fail++;return false;}
    if(transmitting||(DMA1_Stream1->CR&DMA_SxCR_EN)) {
        if(count!=64U)link_diagnostics.tx_busy++;
        return false;
    }
    memcpy(tx_dma,p,count);__DMB();
    GPIOE->BSRR=GPIO_PIN_3; /* Receiver disabled: no transmit echo into RX DMA. */
    GPIOD->BSRR=GPIO_PIN_14;
    /* Two microseconds driver-enable guard is bounded and is only 480 CPU
     * cycles. Turnaround must still be checked at actual cable length. */
    uint32_t start=DWT->CYCCNT;
    while(DWT->CYCCNT-start<C77_CPU_HZ/500000U) __NOP();
    transmitting=true;update_tx=count==64U;update_tx_ok=false;
    USART3->CR1&=~USART_CR1_TCIE;
    DMA1->LIFCR=DMA_LIFCR_CFEIF1|DMA_LIFCR_CDMEIF1|DMA_LIFCR_CTEIF1|DMA_LIFCR_CHTIF1|DMA_LIFCR_CTCIF1;
    DMA1_Stream1->M0AR=(uint32_t)tx_dma;DMA1_Stream1->NDTR=count;
    USART3->ICR=USART_ICR_TCCF;
    DMA1_Stream1->CR|=DMA_SxCR_EN;USART3->CR3|=USART_CR3_DMAT;
    return true;
}
#if !ROLE_LEFT
static void arm_reply_guard(void)
{
    /* Only counter overflow may request the update IRQ (URS). UG still reloads
     * the prescaler. Mask UIE and clear both peripheral/NVIC state so an old
     * request cannot transmit before the eight-microsecond guard has elapsed.
     * Keep this explicit: fast CRC must not remove the master's TC/RX margin
     * when its priority-1 ADC interrupt delays the priority-2 UART handler. */
    TIM4->DIER&=~TIM_DIER_UIE;
    TIM4->CR1=TIM_CR1_URS;TIM4->CNT=0;TIM4->ARR=7U;
    TIM4->EGR=TIM_EGR_UG;TIM4->SR=0;
    __DSB();HAL_NVIC_ClearPendingIRQ(TIM4_IRQn);
    TIM4->DIER|=TIM_DIER_UIE;
    TIM4->CR1=TIM_CR1_URS|TIM_CR1_OPM|TIM_CR1_CEN;
}
#endif
static void packet_received(const uint8_t *p)
{
    if(!allowed) return;
#if ROLE_LEFT
    if(!awaiting||c77_split_sequence(p)!=expected) {errors++;link_diagnostics.sequence_errors++;return;}
    c77_sample_t sample;
    if(!c77_split_decode(p,&sample)) {errors++;link_diagnostics.sequence_errors++;return;}
    awaiting=false;
    uint32_t rtt=board_micros()-sent_at;
    if(rtt<link_diagnostics.rtt_min_us)link_diagnostics.rtt_min_us=rtt;
    if(rtt>link_diagnostics.rtt_max_us)link_diagnostics.rtt_max_us=rtt;
    if(rtt>110U) {errors++;link_diagnostics.late_replies++;return;}
    /* A stopped ADC must not become fresh merely because UART replies recur. */
    if(have_sequence&&sample.sequence==last_sequence) return;
    if(have_sequence&&sample.sequence-last_sequence>=0x80000000U) {
        remote.valid=0;have_sequence=false;errors++;link_diagnostics.sequence_errors++;return;
    }
    have_sequence=true;last_sequence=sample.sequence;
    sample.timestamp_us=board_micros();remote=sample;__DMB();
    if(sample.valid)link_diagnostics.accepted++;
#else
    link_diagnostics.polls++;
    if(transmitting||reply_pending) {errors++;link_diagnostics.tx_busy++;return;}
    reply_sequence=c77_split_sequence(p);reply_pending=true;
    /* IDLE already costs one UART frame. Add a bounded 8us guard so the
     * master's TC IRQ has released DE before the slave starts driving. */
    arm_reply_guard();
#endif
}
static void consume_rx(void)
{
    uint32_t head=(RX_SIZE-DMA1_Stream0->NDTR)&(RX_SIZE-1U);__DMB();
    uint8_t packet[32];
    for(unsigned n=0;tail!=head&&n<RX_SIZE;n++) {
        uint8_t byte=rx_dma[tail];tail=(tail+1U)&(RX_SIZE-1U);
        link_diagnostics.rx_bytes++;
        /* Only start a C7 frame at a normal-frame boundary. Never interpret
         * its 64-byte payload as S7 requests, even when it contains that magic. */
#if HARDWARE_REVISION == 86
        if(update_parser.used || (parser.used==0 && byte=='C')) {
            uint8_t firmware_packet[64];
            if(update_frame_feed(&update_parser,byte,firmware_packet)) {
                if(!update_pending){memcpy(update_packet,firmware_packet,64);update_received_at=board_micros();__DMB();update_pending=true;}
                else errors++;
            }
            continue;
        }
        if(update_mode)continue;
#endif
        if(c77_split_feed(&parser,byte,packet)) packet_received(packet);
    }
}
void USART3_IRQHandler(void)
{
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_BEGIN(cpu_irq);
#endif
    const uint32_t entered=DWT->CYCCNT;link_diagnostics.uart_irqs++;
    uint32_t s=USART3->ISR;
    if(s&(USART_ISR_ORE|USART_ISR_FE|USART_ISR_NE|USART_ISR_PE)) {
        USART3->ICR=USART_ICR_ORECF|USART_ICR_FECF|USART_ICR_NECF|USART_ICR_PECF;
        parser.used=0;update_parser.used=0;errors++;link_diagnostics.uart_errors++;
        abort_tx();
    }
    if((s&USART_ISR_TC)&&(USART3->CR1&USART_CR1_TCIE)) {
        USART3->CR1&=~USART_CR1_TCIE;USART3->CR3&=~USART_CR3_DMAT;
        USART3->ICR=USART_ICR_TCCF;transmitting=false;
        if(update_tx&&active&&allowed)update_tx_ok=true;
        /* TC, not DMA completion: last stop bit has left the transceiver. */
        receive_mode();
    }
    if(s&USART_ISR_IDLE) {USART3->ICR=USART_ICR_IDLECF;consume_rx();}
    const uint32_t elapsed=DWT->CYCCNT-entered;
    if(elapsed>link_diagnostics.uart_irq_max_cycles)link_diagnostics.uart_irq_max_cycles=elapsed;
    __DSB();
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_IRQ_END(cpu_irq,CPU_PROF_IRQ_SPLIT);
#endif
}
void DMA1_Stream0_IRQHandler(void)
{
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_BEGIN(cpu_irq);
#endif
    uint32_t s=DMA1->LISR;
    DMA1->LIFCR=DMA_LIFCR_CFEIF0|DMA_LIFCR_CDMEIF0|DMA_LIFCR_CTEIF0|DMA_LIFCR_CHTIF0|DMA_LIFCR_CTCIF0;
    if(s&(DMA_LISR_TEIF0|DMA_LISR_DMEIF0)) {
        errors++;link_diagnostics.tx_fail++;active=false;abort_tx();remote.valid=0;USART3->CR3&=~USART_CR3_DMAR;
    } else consume_rx();
    __DSB();
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_IRQ_END(cpu_irq,CPU_PROF_IRQ_SPLIT);
#endif
}
void DMA1_Stream1_IRQHandler(void)
{
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_BEGIN(cpu_irq);
#endif
    uint32_t s=DMA1->LISR;
    DMA1->LIFCR=DMA_LIFCR_CFEIF1|DMA_LIFCR_CDMEIF1|DMA_LIFCR_CTEIF1|DMA_LIFCR_CHTIF1|DMA_LIFCR_CTCIF1;
    if((s&(DMA_LISR_TEIF1|DMA_LISR_DMEIF1))||
       ((s&DMA_LISR_TCIF1)&&(!transmitting||DMA1_Stream1->NDTR||(DMA1_Stream1->CR&DMA_SxCR_EN)))) {
        errors++;link_diagnostics.tx_fail++;active=false;remote.valid=0;
        abort_tx();
    } else if(s&DMA_LISR_TCIF1) {
        USART3->CR3&=~USART_CR3_DMAT;USART3->CR1|=USART_CR1_TCIE;
    }
    __DSB();
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_IRQ_END(cpu_irq,CPU_PROF_IRQ_SPLIT);
#endif
}
void TIM4_IRQHandler(void)
{
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_BEGIN(cpu_irq);
#endif
    const uint32_t entered=DWT->CYCCNT;link_diagnostics.poll_irqs++;
    TIM4->SR=0;
    if(!active||!allowed||update_mode) goto finished;
#if ROLE_LEFT
    link_diagnostics.polls++;
    consume_rx();
    if(awaiting) {errors++;link_diagnostics.timeouts++;awaiting=false;}
    if(transmitting) {errors++;link_diagnostics.tx_busy++;goto finished;}
    uint8_t request[8];expected=++next_sequence;
    c77_split_request(request,expected);
    sent_at=board_micros();awaiting=start_tx(request,sizeof request);
#else
    if(reply_pending) {
        reply_pending=false;
        c77_sample_t sample;adc_scan_snapshot(&sample);
        uint8_t reply[32];c77_split_reply(reply,reply_sequence,&sample);
        if(!start_tx(reply,sizeof reply)) errors++;
        else link_diagnostics.accepted++;
    }
#endif
finished:;
    const uint32_t elapsed=DWT->CYCCNT-entered;
    if(elapsed>link_diagnostics.poll_irq_max_cycles)link_diagnostics.poll_irq_max_cycles=elapsed;
    __DSB();
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_IRQ_END(cpu_irq,CPU_PROF_IRQ_SPLIT);
#endif
}
void split_link_start(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();__HAL_RCC_USART3_CLK_ENABLE();__HAL_RCC_TIM4_CLK_ENABLE();
    GPIO_InitTypeDef g={0};g.Pin=GPIO_PIN_8|GPIO_PIN_9;g.Mode=GPIO_MODE_AF_PP;
    g.Speed=GPIO_SPEED_FREQ_VERY_HIGH;g.Pull=GPIO_PULLUP;g.Alternate=GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOD,&g);
    if((SCB->CCR&SCB_CCR_DC_Msk)||(uintptr_t)rx_dma<0x24000000UL||
       (uintptr_t)tx_dma+sizeof tx_dma>0x24050000UL) Error_Handler();
    memset((void *)rx_dma,0,sizeof rx_dma);memset(tx_dma,0,sizeof tx_dma);
    memset(&parser,0,sizeof parser);parser.reply=ROLE_LEFT;
    memset(&update_parser,0,sizeof update_parser);update_mode=false;update_pending=false;
    update_tx=update_tx_ok=false;update_received_at=board_micros();
    tail=0;errors=0;allowed=false;transmitting=false;remote.valid=0;
    memset((void*)&link_diagnostics,0,sizeof link_diagnostics);
    link_diagnostics.rtt_min_us=UINT32_MAX;
    USART3->CR1=0;USART3->CR2=0;USART3->CR3=0;USART3->PRESC=0;
    USART3->BRR=0x14U; /* 120MHz PLL2Q, OVER8, exactly 10Mbps. */
    DMAMUX1_Channel0->CCR=DMA_REQUEST_USART3_RX;DMAMUX1_Channel1->CCR=DMA_REQUEST_USART3_TX;
    DMA1_Stream0->PAR=(uint32_t)&USART3->RDR;DMA1_Stream0->M0AR=(uint32_t)rx_dma;
    DMA1_Stream0->NDTR=RX_SIZE;DMA1_Stream0->FCR=0;
    DMA1_Stream0->CR=DMA_SxCR_MINC|DMA_SxCR_CIRC|DMA_SxCR_PL_1|DMA_SxCR_HTIE|
        DMA_SxCR_TCIE|DMA_SxCR_TEIE|DMA_SxCR_DMEIE;
    DMA1_Stream1->PAR=(uint32_t)&USART3->TDR;DMA1_Stream1->FCR=0;
    DMA1_Stream1->CR=DMA_SxCR_MINC|DMA_SxCR_DIR_0|DMA_SxCR_PL_1|DMA_SxCR_TCIE|DMA_SxCR_TEIE|DMA_SxCR_DMEIE;
    DMA1->LIFCR=0x0f7d0f7dU;DMA1_Stream0->CR|=DMA_SxCR_EN;
    USART3->CR3=USART_CR3_DMAR|USART_CR3_EIE;
    USART3->CR1=USART_CR1_OVER8|USART_CR1_TE|USART_CR1_RE|USART_CR1_IDLEIE|USART_CR1_UE;
    HAL_NVIC_SetPriority(USART3_IRQn,2,0);HAL_NVIC_EnableIRQ(USART3_IRQn);
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn,2,0);HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn,2,0);HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
    TIM4->CR1=0;TIM4->PSC=119U;TIM4->ARR=124U;TIM4->CNT=0;
    TIM4->EGR=TIM_EGR_UG;TIM4->SR=0;TIM4->DIER=TIM_DIER_UIE;
    HAL_NVIC_SetPriority(TIM4_IRQn,2,0);HAL_NVIC_EnableIRQ(TIM4_IRQn);
    active=true;receive_mode();
#if ROLE_LEFT
    next_sequence=0;awaiting=false;have_sequence=false;TIM4->CR1=TIM_CR1_CEN;
#else
    reply_pending=false;
#endif
}
void split_link_allow(bool enable)
{
    uint32_t old=__get_BASEPRI();__set_BASEPRI_MAX(2U<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    if(enable!=allowed) {
        allowed=enable;remote.valid=0;parser.used=0;
        update_parser.used=0;update_pending=false;
        tail=(RX_SIZE-DMA1_Stream0->NDTR)&(RX_SIZE-1U);
        if(!enable) {
            abort_tx();
        }
#if ROLE_LEFT
        awaiting=false;have_sequence=false;
#else
        reply_pending=false;
#endif
        receive_mode();
    }
    __set_BASEPRI(old);
}
void split_update_mode(bool enable)
{
    uint32_t old=__get_BASEPRI();__set_BASEPRI_MAX(2U<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    update_mode=enable;parser.used=0;update_parser.used=0;update_pending=false;remote.valid=0;
    update_mode_at=board_micros();
    TIM4->CR1=0;TIM4->SR=0;
#if ROLE_LEFT
    awaiting=false;have_sequence=false;
    if(!enable&&active){TIM4->CNT=0;TIM4->CR1=TIM_CR1_CEN;}
#else
    reply_pending=false;
#endif
    __set_BASEPRI(old);
}
bool split_update_send(const uint8_t packet[64])
{
    if(!packet||!update_frame_valid(packet)||(uint32_t)(board_micros()-update_received_at)<3U||
       (update_mode&&(uint32_t)(board_micros()-update_mode_at)<150U))return false;
    uint32_t old=__get_BASEPRI();__set_BASEPRI_MAX(2U<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    bool result=start_tx(packet,64);
    if(result){update_tx=true;update_tx_ok=false;}
    __set_BASEPRI(old);return result;
}
bool split_update_read(uint8_t packet[64])
{
    uint32_t old=__get_BASEPRI();__set_BASEPRI_MAX(2U<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    bool result=update_pending;
    if(result){memcpy(packet,update_packet,64);update_pending=false;}
    __set_BASEPRI(old);return result;
}
bool split_update_tx_idle(void){return !transmitting;}
bool split_update_tx_succeeded(void){return update_tx_ok;}
bool split_link_snapshot(c77_sample_t *out)
{
    if(!out) return false;
    uint32_t old=__get_BASEPRI();__set_BASEPRI_MAX(2U<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    *out=remote;out->errors+=errors+parser.crc_errors;
    if(!active||!allowed||!out->sequence||board_micros()-out->timestamp_us>2000U) out->valid=0;
    __set_BASEPRI(old);
    return out->valid!=0;
}
void split_link_stop(void)
{
    split_link_allow(false);active=false;
    TIM4->CR1=0;TIM4->DIER=0;TIM4->SR=0;
    HAL_NVIC_DisableIRQ(TIM4_IRQn);HAL_NVIC_DisableIRQ(USART3_IRQn);
    HAL_NVIC_DisableIRQ(DMA1_Stream0_IRQn);HAL_NVIC_DisableIRQ(DMA1_Stream1_IRQn);
    USART3->CR1=0;USART3->CR3=0;
    DMA1_Stream0->CR&=~DMA_SxCR_EN;DMA1_Stream1->CR&=~DMA_SxCR_EN;
    uint32_t start=board_micros();
    while((DMA1_Stream0->CR|DMA1_Stream1->CR)&DMA_SxCR_EN)
        if(board_micros()-start>100U) Error_Handler();
    receive_mode();
}
void split_link_stats(split_link_stats_t *out)
{
    if(!out)return;
    uint32_t old=__get_BASEPRI();__set_BASEPRI_MAX(2U<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    *out=link_diagnostics;
    out->flags=(active?1U:0U)|(allowed?2U:0U)|(transmitting?4U:0U)|(update_mode?8U:0U);
    out->crc_errors=parser.crc_errors;out->framing_errors=parser.framing_errors;
    if(out->rtt_min_us==UINT32_MAX)out->rtt_min_us=0;
    __set_BASEPRI(old);
}
