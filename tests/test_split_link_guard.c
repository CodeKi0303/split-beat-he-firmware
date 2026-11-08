#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "split_codec.h"
#include "split_link_diag.h"

#define TIM_DIER_UIE 1U
#define TIM_EGR_UG 1U
#define TIM_CR1_CEN 1U
#define TIM_CR1_URS 4U
#define TIM_CR1_OPM 8U
#define TIM4_IRQn 30U
#define __NVIC_PRIO_BITS 4U
#define __DMB() ((void)0)
#define __DSB() ((void)0)
#define __ISB() ((void)0)
typedef struct {uint32_t CR1,CNT,ARR,EGR,SR,DIER;} timer_mock_t;
static timer_mock_t timer;
#define TIM4 (&timer)
static struct {uint32_t CYCCNT;} cycles;
#define DWT (&cycles)
static bool active,allowed,transmitting,update_mode;
static uint32_t errors,now_us,basepri,tx_count,last_tx_size;
static uint8_t last_tx[32];
static volatile split_link_stats_t link_diagnostics;
static c77_split_parser_t parser;
#if ROLE_LEFT
static volatile c77_sample_t remote;
static bool awaiting,have_sequence;
static uint16_t next_sequence,expected;
static uint32_t sent_at,last_sequence,consume_calls;
static void consume_rx(void) {consume_calls++;}
#else
static bool reply_pending,nvic_pending;
static uint16_t reply_sequence;
static uint32_t pending_clears;
static void HAL_NVIC_ClearPendingIRQ(uint32_t irq)
{
    assert(irq==TIM4_IRQn);
    /* This observes the actual handler's ordering, not a duplicate guard. */
    assert(!(TIM4->DIER&TIM_DIER_UIE));
    assert(!(TIM4->CR1&TIM_CR1_CEN));
    assert(TIM4->CR1&TIM_CR1_URS);
    assert(TIM4->SR==0 && TIM4->EGR==TIM_EGR_UG);
    nvic_pending=false;pending_clears++;
}
static void adc_scan_snapshot(c77_sample_t *sample)
{
    memset(sample,0,sizeof *sample);sample->sequence=123U;sample->valid=1;
}
#endif
uint32_t board_micros(void) {return now_us;}
static uint32_t __get_BASEPRI(void) {return basepri;}
static void __set_BASEPRI_MAX(uint32_t value)
{
    if(!basepri||value<basepri)basepri=value;
}
static void __set_BASEPRI(uint32_t value) {basepri=value;}
static bool start_tx(const uint8_t *packet,unsigned count)
{
    assert(active&&allowed&&!transmitting);
    assert(count<=sizeof last_tx);
    memcpy(last_tx,packet,count);last_tx_size=count;tx_count++;
    DWT->CYCCNT+=100U; /* Synthetic duration, including a counter wrap. */
    transmitting=true;return true;
}
#include "split_link_under_test.inc"

static void reset_test(void)
{
    memset(&timer,0,sizeof timer);memset(&parser,0,sizeof parser);
    memset((void*)&link_diagnostics,0,sizeof link_diagnostics);
    link_diagnostics.rtt_min_us=UINT32_MAX;
    active=allowed=true;transmitting=update_mode=false;
    errors=now_us=basepri=tx_count=last_tx_size=0;DWT->CYCCNT=0;
#if ROLE_LEFT
    memset((void*)&remote,0,sizeof remote);
    awaiting=have_sequence=false;next_sequence=expected=0;
    sent_at=last_sequence=consume_calls=0;
#else
    reply_pending=nvic_pending=false;reply_sequence=0;pending_clears=0;
#endif
}
static void test_snapshot(void)
{
    reset_test();split_link_stats_t stats;
    parser.crc_errors=7;parser.framing_errors=9;
    link_diagnostics.polls=17;link_diagnostics.uart_errors=2;
    basepri=16;split_link_stats(&stats);
    assert(basepri==16 && stats.polls==17 && stats.uart_errors==2);
    assert(stats.crc_errors==7 && stats.framing_errors==9);
    assert(!stats.rtt_min_us&&!stats.rtt_max_us);
    assert(stats.flags==3U);
    transmitting=update_mode=true;split_link_stats(&stats);assert(stats.flags==15U);
    active=allowed=false;split_link_stats(&stats);assert(stats.flags==12U);
    split_link_stats(NULL);assert(basepri==16);
}
#if ROLE_LEFT
static void deliver(uint16_t sequence,uint32_t adc_sequence,uint32_t rtt)
{
    uint8_t reply[32];c77_sample_t sample={0};
    sample.sequence=adc_sequence;sample.valid=1;
    c77_split_reply(reply,sequence,&sample);
    now_us=sent_at+rtt;packet_received(reply);
}
static void test_reply_classification(void)
{
    reset_test();awaiting=true;expected=7;sent_at=100;
    deliver(7,1000,40);
    assert(!awaiting&&remote.valid&&remote.sequence==1000);
    assert(link_diagnostics.accepted==1&&!errors);
    assert(link_diagnostics.rtt_min_us==40&&link_diagnostics.rtt_max_us==40);
    deliver(7,1001,41); /* Duplicate/unexpected transaction. */
    assert(errors==1&&link_diagnostics.sequence_errors==1);
    awaiting=true;expected=8;deliver(9,1001,42);
    assert(awaiting&&link_diagnostics.sequence_errors==2);
    deliver(8,1001,111);
    assert(!awaiting&&link_diagnostics.late_replies==1);
    assert(remote.sequence==1000&&link_diagnostics.accepted==1);
    assert(link_diagnostics.rtt_max_us==111);
    awaiting=true;deliver(8,1000,35); /* ADC stale despite successful UART. */
    assert(link_diagnostics.accepted==1&&remote.sequence==1000);
    awaiting=true;deliver(8,999,36);
    assert(!remote.valid&&!have_sequence&&link_diagnostics.sequence_errors==3);
    awaiting=true;sent_at=UINT32_MAX-10;deliver(8,1002,30);
    assert(remote.valid&&link_diagnostics.accepted==2);
    assert(link_diagnostics.rtt_min_us==30);
}
static void test_poll_timeout(void)
{
    reset_test();now_us=1000;DWT->CYCCNT=UINT32_MAX-50U;TIM4_IRQHandler();
    assert(tx_count==1&&last_tx_size==8&&awaiting&&expected==1);
    assert(link_diagnostics.polls==1&&!link_diagnostics.timeouts);
    assert(link_diagnostics.poll_irqs==1&&link_diagnostics.poll_irq_max_cycles==100U);
    transmitting=false;now_us+=125;TIM4_IRQHandler();
    assert(tx_count==2&&expected==2&&link_diagnostics.timeouts==1);
    TIM4_IRQHandler();
    assert(tx_count==2&&link_diagnostics.timeouts==2&&link_diagnostics.tx_busy==1);
    update_mode=true;TIM4_IRQHandler();assert(link_diagnostics.polls==3);
    update_mode=false;transmitting=false;link_diagnostics.polls=UINT32_MAX;
    TIM4_IRQHandler();assert(link_diagnostics.polls==0);
    assert(consume_calls==4);
    link_diagnostics.poll_irqs=UINT32_MAX;allowed=false;TIM4_IRQHandler();
    assert(link_diagnostics.poll_irqs==0&&link_diagnostics.poll_irq_max_cycles==100U);
}
#else
static void advance_us(void)
{
    now_us++;
    if(TIM4->CR1&TIM_CR1_CEN) {
        if(TIM4->CNT++>=TIM4->ARR) {
            TIM4->SR=1;TIM4->CR1&=~TIM_CR1_CEN;
            if(TIM4->DIER&TIM_DIER_UIE)nvic_pending=true;
        }
    }
    if(nvic_pending){nvic_pending=false;TIM4_IRQHandler();}
}
static void test_guard_pending(void)
{
    reset_test();uint8_t request[8];c77_split_request(request,57);
    TIM4->DIER=TIM_DIER_UIE;TIM4->SR=1;nvic_pending=true;
    packet_received(request);
    assert(pending_clears==1&&!nvic_pending&&reply_pending);
    assert(!tx_count&&(TIM4->DIER&TIM_DIER_UIE));
    assert(TIM4->CR1==(TIM_CR1_URS|TIM_CR1_OPM|TIM_CR1_CEN));
    for(unsigned elapsed=1;elapsed<8;elapsed++) {
        advance_us();assert(!tx_count&&reply_pending);
    }
    advance_us();assert(tx_count==1&&!reply_pending);
    assert(last_tx_size==32&&c77_split_sequence(last_tx)==57);
    c77_sample_t sample;assert(c77_split_decode(last_tx,&sample));
    assert(sample.sequence==123&&sample.valid);
    assert(link_diagnostics.polls==1&&link_diagnostics.accepted==1);
    advance_us();assert(tx_count==1);
}
static void test_reply_busy(void)
{
    reset_test();uint8_t request[8];c77_split_request(request,58);
    TIM4->DIER=TIM_DIER_UIE;packet_received(request);
    packet_received(request);
    assert(link_diagnostics.polls==2&&link_diagnostics.tx_busy==1&&errors==1);
    assert(pending_clears==1);
    update_mode=true;
    for(unsigned elapsed=0;elapsed<8;elapsed++)advance_us();
    assert(!tx_count);
    allowed=false;packet_received(request);assert(link_diagnostics.polls==2);
}
#endif
int main(void)
{
    test_snapshot();
#if ROLE_LEFT
    test_reply_classification();test_poll_timeout();
#else
    test_guard_pending();test_reply_busy();
#endif
    printf("split-link %s source-handler tests passed\n",ROLE_LEFT?"left":"right");
    return 0;
}
