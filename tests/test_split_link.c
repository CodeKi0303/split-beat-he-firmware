/* Real split-link DMA/IRQ/main handshake with observed registers. Never opens
 * hardware; GPIO writes model commands, not external transceiver timing. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "hardware_profile.h"
#include "split_codec.h"
#include "split_link_diag.h"
#include "update_link.h"
#ifdef _MSC_VER
#define __attribute__(...)
#pragma warning(disable:4311 4302)
#endif
#if defined(__GNUC__)
/* The production cast targets a 32-bit MCU address register. Only the host
 * observer has 64-bit pointers; the ARM build checks the unmodified source. */
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
#endif
#define C77_CPU_HZ 240000000U
#define __NVIC_PRIO_BITS 4U
static struct { uint32_t BSRR; } port_d,port_e;
static struct { uint32_t CR,NDTR; uintptr_t M0AR; } stream0,stream1;
static struct { uint32_t LISR,LIFCR; } dma;
static struct { uint32_t CR1,CR3,ISR,ICR; } usart;
static struct { uint32_t CR1,CR2,CNT,ARR,EGR,SR,DIER; } timer;
static struct { uint32_t CYCCNT; } cycles;
#define GPIOD (&port_d)
#define GPIOE (&port_e)
#define DMA1 (&dma)
#define DMA1_Stream0 (&stream0)
#define DMA1_Stream1 (&stream1)
#define USART3 (&usart)
#define TIM4 (&timer)
#define DWT (&cycles)
#define GPIO_PIN_3 (1U<<3)
#define GPIO_PIN_14 (1U<<14)
#define DMA_SxCR_EN 1U
#define USART_CR1_TCIE 1U
#define USART_CR3_DMAT 1U
#define USART_CR3_DMAR 2U
#define USART_ICR_TCCF 1U
#define USART_ICR_ORECF 2U
#define USART_ICR_FECF 4U
#define USART_ICR_NECF 8U
#define USART_ICR_PECF 16U
#define USART_ICR_IDLECF 32U
#define USART_ISR_TC 1U
#define USART_ISR_ORE 2U
#define USART_ISR_FE 4U
#define USART_ISR_NE 8U
#define USART_ISR_PE 16U
#define USART_ISR_IDLE 32U
#define DMA_LISR_TEIF0 1U
#define DMA_LISR_DMEIF0 2U
#define DMA_LISR_TEIF1 4U
#define DMA_LISR_DMEIF1 8U
#define DMA_LISR_TCIF1 16U
#define DMA_LISR_HTIF0 32U
#define DMA_LIFCR_CFEIF0 1U
#define DMA_LIFCR_CDMEIF0 2U
#define DMA_LIFCR_CTEIF0 4U
#define DMA_LIFCR_CHTIF0 8U
#define DMA_LIFCR_CTCIF0 16U
#define DMA_LIFCR_CFEIF1 32U
#define DMA_LIFCR_CDMEIF1 64U
#define DMA_LIFCR_CTEIF1 128U
#define DMA_LIFCR_CHTIF1 256U
#define DMA_LIFCR_CTCIF1 512U
#define TIM_EGR_UG 1U
#define TIM_DIER_UIE 1U
#define TIM_CR1_CEN 1U
#define TIM_CR1_URS 4U
#define TIM_CR1_OPM 2U
#define TIM4_IRQn 1U
#define USART3_IRQn 2U
#define DMA1_Stream0_IRQn 3U
#define DMA1_Stream1_IRQn 4U
#define __DSB() ((void)0)
#define __ISB() ((void)0)
#define __DMB() ((void)0)
#define __NOP() (++cycles.CYCCNT)
static uint32_t basepri,now,cycle_step_per_time;
static uint32_t __get_BASEPRI(void) { return basepri; }
static void __set_BASEPRI(uint32_t value) { basepri=value; }
static void __set_BASEPRI_MAX(uint32_t value) { if(!basepri||value<basepri)basepri=value; }
static void HAL_NVIC_DisableIRQ(unsigned irq) { (void)irq; }
#if !ROLE_LEFT
static void HAL_NVIC_ClearPendingIRQ(unsigned irq) { assert(irq==TIM4_IRQn); }
#endif
uint32_t board_micros(void) { DWT->CYCCNT+=cycle_step_per_time;return now; }
void Error_Handler(void) { assert(!"Unexpected Error_Handler"); }
#if !ROLE_LEFT
static bool adc_scan_snapshot(c77_sample_t *sample)
{ memset(sample,0,sizeof *sample);sample->sequence=1;sample->valid=1;return true; }
#endif

/* Function bodies and state extracted without rewriting logic. Startup GPIO
 * and clock programming are separately covered by board/ARM build checks. */
#include "../build/host/split_link_under_test.inc"

static void reset(void)
{
    memset(&port_d,0,sizeof port_d);memset(&port_e,0,sizeof port_e);
    memset(&stream0,0,sizeof stream0);memset(&stream1,0,sizeof stream1);
    memset(&dma,0,sizeof dma);memset(&usart,0,sizeof usart);memset(&timer,0,sizeof timer);
    memset(&cycles,0,sizeof cycles);memset((void *)rx_dma,0,sizeof rx_dma);
    memset(tx_dma,0,sizeof tx_dma);memset(&parser,0,sizeof parser);
    memset((void *)&remote,0,sizeof remote);memset(&update_parser,0,sizeof update_parser);
    tail=0;errors=0;active=allowed=true;transmitting=false;
    memset((void *)&link_diagnostics,0,sizeof link_diagnostics);
    link_diagnostics.rtt_min_us=UINT32_MAX;
    update_mode=false;update_pending=false;update_tx=update_tx_ok=false;
    update_received_at=0;basepri=0;now=1000U;cycle_step_per_time=0;parser.reply=ROLE_LEFT;
    stream0.NDTR=RX_SIZE;
#if ROLE_LEFT
    awaiting=false;have_sequence=false;next_sequence=0;expected=0;sent_at=last_sequence=0;
#else
    reply_pending=false;reply_sequence=0;
#endif
}
static void firmware_packet(uint8_t packet[64])
{
    memset(packet,0,64);packet[0]='C';packet[1]='7';packet[2]=1;packet[3]=0x33;
    packet[4]=11;packet[7]=8;c77_split_request(packet+8,123U);update_frame_finish(packet);
}
static void inject(const uint8_t *bytes,unsigned count)
{
    uint32_t head=(RX_SIZE-stream0.NDTR)&(RX_SIZE-1U);
    for(unsigned i=0;i<count;++i){rx_dma[head]=bytes[i];head=(head+1U)&(RX_SIZE-1U);}
    stream0.NDTR=RX_SIZE-head;dma.LISR=DMA_LISR_HTIF0;DMA1_Stream0_IRQHandler();
}
static void dma_done(void)
{ stream1.CR&=~DMA_SxCR_EN;stream1.NDTR=0;dma.LISR=DMA_LISR_TCIF1;DMA1_Stream1_IRQHandler(); }
static void uart_done(uint32_t extra)
{ usart.ISR=USART_ISR_TC|extra;USART3_IRQHandler();usart.ISR=0; }
static void test_frame_boundaries(void)
{
    uint8_t frame[64],readback[64];firmware_packet(frame);
    reset();inject(frame,31U);assert(!update_pending && update_parser.used==31U);
    inject(frame+31U,33U);assert(split_update_read(readback));assert(!memcmp(frame,readback,64));
    assert(!split_update_read(readback));
#if !ROLE_LEFT
    assert(!reply_pending && !(TIM4->CR1&TIM_CR1_CEN)); /* Payload S7 is inert. */
#endif
    inject(frame,64U);assert(update_pending);inject(frame,64U);assert(errors==1U);
    assert(split_update_read(readback));
    reset();
#if ROLE_LEFT
    c77_sample_t sample={0};sample.sequence=0x13743U;sample.valid=1U;
    c77_split_reply(frame,44U,&sample);awaiting=true;expected=44U;sent_at=now;
    inject(frame,32U);assert(remote.valid && remote.sequence==sample.sequence);
#else
    c77_split_request(frame,0x3743U);inject(frame,8U);
    assert(reply_pending && reply_sequence==0x3743U);
#endif
    assert(!update_pending && update_parser.used==0U);
}
static void test_dma_is_not_wire_ack(void)
{
    uint8_t packet[64];firmware_packet(packet);reset();
    assert(split_update_send(packet));assert(transmitting && !split_update_tx_succeeded());
    assert(port_d.BSRR==GPIO_PIN_14 && port_e.BSRR==GPIO_PIN_3);
    assert(cycles.CYCCNT>=480U && stream1.NDTR==64U);
    assert(!memcmp(tx_dma,packet,64));assert(!split_update_send(packet));
    dma_done();assert(transmitting && !split_update_tx_idle() && !split_update_tx_succeeded());
    assert(usart.CR1&USART_CR1_TCIE);
    uart_done(0);assert(split_update_tx_idle() && split_update_tx_succeeded());
    assert(port_d.BSRR==((uint32_t)GPIO_PIN_14<<16));
    assert(port_e.BSRR==((uint32_t)GPIO_PIN_3<<16));
}
static void test_aborts_never_become_ack(void)
{
    uint8_t packet[64];firmware_packet(packet);
    reset();assert(split_update_send(packet));dma_done();
    dma.LISR=DMA_LISR_TEIF0;DMA1_Stream0_IRQHandler();uart_done(0);
    assert(!active && split_update_tx_idle() && !split_update_tx_succeeded());
    assert(!(usart.CR3&USART_CR3_DMAT) && !(usart.CR1&USART_CR1_TCIE));
    reset();assert(split_update_send(packet));dma_done();uart_done(0);
    dma.LISR=DMA_LISR_TEIF0;DMA1_Stream0_IRQHandler();
    assert(!split_update_tx_succeeded()); /* Error before main consumes ACK. */
    reset();assert(split_update_send(packet));dma.LISR=DMA_LISR_TEIF1;DMA1_Stream1_IRQHandler();
    uart_done(0);assert(!active && split_update_tx_idle() && !split_update_tx_succeeded());
    reset();assert(split_update_send(packet));dma_done();uart_done(USART_ISR_ORE);
    assert(split_update_tx_idle() && !split_update_tx_succeeded());
    reset();assert(split_update_send(packet));dma_done();split_link_allow(false);uart_done(0);
    assert(split_update_tx_idle() && !split_update_tx_succeeded());
}
static void test_mode_switch_and_reply_guard(void)
{
    uint8_t packet[64];firmware_packet(packet);reset();
    TIM4->CR1=TIM_CR1_CEN;split_update_mode(true);
    assert(update_mode && !(TIM4->CR1&TIM_CR1_CEN));
    assert(!split_update_send(packet));now+=149U;assert(!split_update_send(packet));
    ++now;assert(split_update_send(packet));dma_done();uart_done(0);
    split_update_mode(false);assert(!update_mode);
#if ROLE_LEFT
    assert(TIM4->CR1&TIM_CR1_CEN);
#else
    assert(!(TIM4->CR1&TIM_CR1_CEN));
#endif
    reset();inject(packet,64U);assert(update_pending);
    assert(!split_update_send(packet));now+=2U;assert(!split_update_send(packet));
    ++now;assert(split_update_send(packet));
    dma_done();uart_done(0);split_link_stop();
    assert(!active && !allowed && !transmitting && !(usart.CR3&USART_CR3_DMAR));
}
static void test_scheduling_diagnostics(void)
{
    reset();uint8_t packet[64];firmware_packet(packet);
    inject(packet,64U);assert(link_diagnostics.rx_bytes==64U);
    /* Every consumed byte counts, including malformed input and uint32 wrap. */
    update_pending=false;link_diagnostics.rx_bytes=UINT32_MAX;
    const uint8_t invalid=0;inject(&invalid,1U);assert(link_diagnostics.rx_bytes==0U);
    reset();cycle_step_per_time=100U;DWT->CYCCNT=UINT32_MAX-50U;
    /* IDLE consumes a complete firmware frame; its timestamp crosses CYCCNT wrap. */
    for(unsigned i=0;i<64U;i++)rx_dma[i]=packet[i];stream0.NDTR=RX_SIZE-64U;
    usart.ISR=USART_ISR_IDLE;USART3_IRQHandler();
    assert(link_diagnostics.uart_irqs==1U&&link_diagnostics.uart_irq_max_cycles==100U);
    assert(link_diagnostics.rx_bytes==64U&&update_pending);
    cycle_step_per_time=0;usart.ISR=0;link_diagnostics.uart_irqs=UINT32_MAX;
    USART3_IRQHandler();assert(link_diagnostics.uart_irqs==0U&&link_diagnostics.uart_irq_max_cycles==100U);
    split_link_stats_t stats;split_link_stats(&stats);assert(stats.flags==3U);
    transmitting=update_mode=true;split_link_stats(&stats);assert(stats.flags==15U);
    active=allowed=false;split_link_stats(&stats);assert(stats.flags==12U);
}
int main(void)
{
    test_frame_boundaries();test_dma_is_not_wire_ack();test_aborts_never_become_ack();
    test_mode_switch_and_reply_guard();test_scheduling_diagnostics();
    printf("C86 %s split link: frame boundary, DMA vs TC, abort, quiet transition and receive guard passed\n",
           ROLE_LEFT?"left":"right");
    return 0;
}
