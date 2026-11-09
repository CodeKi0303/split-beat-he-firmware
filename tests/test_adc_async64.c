/* Real async acquisition logic with synthetic DMA registers, no hardware. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "adc_scan_private.h"
#if !ROLE_LEFT
#include "split_codec.h"
#endif
#define C77_CPU_HZ 240000000U
#ifdef _MSC_VER
#define __attribute__(attributes)
#endif
typedef struct {uint32_t ISR;} ADC_TypeDef;
typedef struct {ADC_TypeDef *Instance;uint32_t ErrorCode,State;} ADC_HandleTypeDef;
typedef struct {uint32_t ErrorCode;} DMA_HandleTypeDef;
typedef struct {volatile uint32_t CR,FCR,NDTR;} DMA_Stream_TypeDef;
typedef struct {void *Instance;} TIM_HandleTypeDef;
static ADC_TypeDef adc_registers[2];
static DMA_Stream_TypeDef streams[2];
static struct {uint32_t CR1;} timer_registers;
static struct {uint32_t LISR,LIFCR;} dma_registers;
static struct {uint32_t CYCCNT;} cycle_registers;
#define DMA2_Stream0 (&streams[0])
#define DMA2_Stream1 (&streams[1])
#define TIM3 (&timer_registers)
#define DMA2 (&dma_registers)
#define DWT (&cycle_registers)
#define TIM_CR1_CEN 1U
#define ADC_IT_OVR 1U
#define ADC_FLAG_OVR 1U
#define DMA_SxCR_EN 1U
#define DMA_SxCR_TCIE 2U
#define DMA_SxCR_HTIE 4U
#define DMA_SxCR_TEIE 8U
#define DMA_SxCR_DMEIE 16U
#define DMA_SxFCR_FEIE 1U
#define DMA_LISR_TEIF0 1U
#define DMA_LISR_DMEIF0 2U
#define DMA_LISR_FEIF0 4U
#define DMA_LISR_HTIF0 8U
#define DMA_LISR_TCIF0 16U
#define DMA_LISR_TEIF1 64U
#define DMA_LISR_DMEIF1 128U
#define DMA_LISR_FEIF1 256U
#define DMA_LISR_HTIF1 512U
#define DMA_LISR_TCIF1 1024U
#define DMA_LIFCR_CHTIF0 DMA_LISR_HTIF0
#define DMA_LIFCR_CTCIF0 DMA_LISR_TCIF0
#define DMA_LIFCR_CHTIF1 DMA_LISR_HTIF1
#define DMA_LIFCR_CTCIF1 DMA_LISR_TCIF1
#define HAL_DMA_ERROR_TE 1U
#define HAL_DMA_ERROR_DME 2U
#define HAL_DMA_ERROR_FE 4U
#define HAL_ADC_ERROR_DMA 1U
#define HAL_ADC_ERROR_OVR 2U
#define HAL_ADC_STATE_ERROR_DMA 1U
#define __NVIC_PRIO_BITS 4U
#define CLEAR_BIT(reg,bits) ((reg)&=~(bits))
static uint32_t enabled_adc_irqs[2],basepri,highest_mask,clock_us,clock_fraction,input_calls,input_errors;
static uint32_t producer[2],barriers,move_copy_frames,move_copy_unit,move_sum_frames;
static bool copy_service,stop_during_sum,copy_was_unmasked;
static uint32_t copy_delay_cycles;
static void clear_written_flags(void) {DMA2->LISR&=~DMA2->LIFCR;DMA2->LIFCR=0U;}
static void advance(unsigned unit,unsigned frames,bool service);
static void memory_barrier(void);
#define __DSB() clear_written_flags()
#define __DMB() memory_barrier()
#define __ISB() ((void)0)
#define __HAL_ADC_DISABLE_IT(adc,bits) (enabled_adc_irqs[(adc)-scan_adc]&=~(bits))
static uint32_t __get_BASEPRI(void) {return basepri;}
static void __set_BASEPRI(uint32_t mask);
static void __set_BASEPRI_MAX(uint32_t mask) {if(!basepri||mask<basepri)basepri=mask;highest_mask=basepri;}
static uint32_t HAL_ADC_GetError(ADC_HandleTypeDef *adc) {return adc->ErrorCode;}
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *adc);
static void HAL_ADC_IRQHandler(ADC_HandleTypeDef *adc)
{if(adc->Instance->ISR&ADC_FLAG_OVR){adc->ErrorCode|=HAL_ADC_ERROR_OVR;HAL_ADC_ErrorCallback(adc);}}
enum {DMA2_Stream0_IRQn=0,DMA2_Stream1_IRQn=1,ADC_IRQn=2};
static uint32_t stopped_adcs,disabled_nvic,cleared_nvic,timer_stop_calls;
static int HAL_TIM_Base_Stop(TIM_HandleTypeDef *timer);
static int HAL_ADC_Stop_DMA(ADC_HandleTypeDef *adc);
static void HAL_NVIC_DisableIRQ(unsigned irq) {disabled_nvic|=1U<<irq;}
static void HAL_NVIC_ClearPendingIRQ(unsigned irq) {cleared_nvic|=1U<<irq;}
uint32_t board_micros(void) {return clock_us;}
static void board_input_scan(c77_sample_t *sample);
#include "adc_async64_under_test.inc"
static int HAL_TIM_Base_Stop(TIM_HandleTypeDef *timer)
{assert(timer==&scan_timer);TIM3->CR1=0U;timer_stop_calls++;return 0;}
static int HAL_ADC_Stop_DMA(ADC_HandleTypeDef *adc)
{stopped_adcs|=1U<<(unsigned)(adc-scan_adc);return 0;}
static void board_input_scan(c77_sample_t *sample)
{sample->digital=5U;sample->encoders[0]=12U;sample->encoders[1]=23U;sample->errors+=input_errors;input_calls++;}
static void __set_BASEPRI(uint32_t mask)
{
    basepri=mask;
    if(barriers==2U && move_sum_frames) {
        const uint32_t frames=move_sum_frames;move_sum_frames=0U;advance(0U,frames,true);
    }
    if(barriers==2U && stop_during_sum) {stop_during_sum=false;halt_from_irq();}
}
static void memory_barrier(void)
{
    clear_written_flags();barriers++;
    if(barriers==1U)copy_was_unmasked=basepri==0U;
    if(barriers==2U) {
        if(move_copy_frames) {
            const uint32_t frames=move_copy_frames;move_copy_frames=0U;
            advance(move_copy_unit,frames,copy_service);
        }
        DWT->CYCCNT+=copy_delay_cycles;
    }
}
static uint16_t value_at(unsigned unit,uint32_t sequence,unsigned rank)
{return (uint16_t)(500U+unit*1000U+(sequence%1000U)*2U+rank);}
static void advance(unsigned unit,unsigned frames,bool service)
{
    for(unsigned i=0;i<frames;i++) {
        const uint32_t seq=producer[unit];
        volatile uint16_t *ring=unit?scan_buffer2.samples:scan_buffer1.samples;
        for(unsigned rank=0;rank<ranks_for(unit);rank++)
            ring[(seq&(ASYNC_RING_SCANS-1U))*ranks_for(unit)+rank]=value_at(unit,seq+1U,rank);
        producer[unit]++;
        const uint32_t pos=producer[unit]&(ASYNC_RING_SCANS-1U);
        stream_for(unit)->NDTR=(ASYNC_RING_SCANS-pos)*ranks_for(unit);
        if(!(producer[unit]&(ASYNC_HALF_SCANS-1U))) {
            DMA2->LISR|=half_flag(unit,(producer[unit]&ASYNC_HALF_SCANS)?0U:1U);
            if(service) {
                assert(basepri==0U || basepri>(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS)));
                if(unit)DMA2_Stream1_IRQHandler();else DMA2_Stream0_IRQHandler();
            }
        }
        if((int32_t)(producer[unit]-producer[unit^1U])>0) {
            DWT->CYCCNT+=ASYNC_SCAN_CYCLES;clock_fraction+=ASYNC_SCAN_CYCLES;
            clock_us+=clock_fraction/(C77_CPU_HZ/1000000U);
            clock_fraction%=C77_CPU_HZ/1000000U;
        }
    }
}
static void reset(void)
{
#if SCAN_ADC_PHASE_PAIR
    active_filter=0U;
#endif
    memset((void*)&scan_state,0,sizeof scan_state);memset((void*)banks,0,sizeof banks);
#if !ROLE_LEFT
    satellite_reset();
#endif
    memset((void*)&scan_buffer1,0,sizeof scan_buffer1);memset((void*)&scan_buffer2,0,sizeof scan_buffer2);
    memset(producer,0,sizeof producer);memset(scan_adc,0,sizeof scan_adc);memset(scan_dma,0,sizeof scan_dma);
    scan_state.running=1U;scan_state.period_min_cycles=UINT32_MAX;
    scan_timer.Instance=&timer_registers;TIM3->CR1=1U;DMA2->LISR=DMA2->LIFCR=DWT->CYCCNT=0U;
    dma_started=previous_boundary_cycle=have_previous_boundary=last_stats_sequence=0U;
    basepri=highest_mask=clock_us=clock_fraction=input_calls=input_errors=barriers=0U;
    move_copy_frames=move_sum_frames=copy_delay_cycles=0U;move_copy_unit=0U;
    copy_service=stop_during_sum=copy_was_unmasked=false;
    stopped_adcs=disabled_nvic=cleared_nvic=timer_stop_calls=0U;
    for(unsigned unit=0;unit<2U;unit++) {
        scan_adc[unit].Instance=&adc_registers[unit];adc_registers[unit].ISR=0U;
        streams[unit].NDTR=ASYNC_RING_SCANS*ranks_for(unit);
        streams[unit].CR=DMA_SxCR_EN|DMA_SxCR_TCIE|DMA_SxCR_HTIE|DMA_SxCR_TEIE|DMA_SxCR_DMEIE;
        streams[unit].FCR=0U;enabled_adc_irqs[unit]=ADC_IT_OVR;
    }
}
static bool snapshot(unsigned unit,c77_sample_t *out)
{barriers=0U;return adc_scan_bank_snapshot(unit,out);}
static void check_mean(unsigned unit,const c77_sample_t *sample,uint32_t sequence)
{
    assert(sample->sequence==sequence && sample->valid && sample->hall_filtered_valid);
    const unsigned first=unit?4U:0U;
    const unsigned key_count=unit?3U:4U;
    for(unsigned rank=0;rank<key_count;rank++) {
        unsigned frames=8U;
#if SCAN_ADC_PHASE_PAIR
        if(active_filter)frames=active_filter;
        if(unit==0U || rank>0U) {
            const unsigned a=unit==0U?2U*rank:2U*rank-1U;
            assert(sample->raw[first+rank]==(value_at(unit,sequence,a)+value_at(unit,sequence,a+1U)+1U)/2U);
            if(!active_filter)frames=1U;
            uint32_t sum=0U;
            for(unsigned i=0;i<frames;i++)sum+=value_at(unit,sequence-i,a)+value_at(unit,sequence-i,a+1U);
            assert(sample->hall_filtered[first+rank]==(sum+frames)/(2U*frames));
            continue;
        }
#endif
        uint32_t sum=0U;
        for(unsigned i=0;i<frames;i++)sum+=value_at(unit,sequence-i,rank);
        assert(sample->raw[first+rank]==value_at(unit,sequence,rank));
        assert(sample->hall_filtered[first+rank]==(sum+frames/2U)/frames);
    }
    for(unsigned rank=0;rank<7U;rank++)if(rank<first||rank>=first+key_count)
        assert(!sample->raw[rank] && !sample->hall_filtered[rank]);
}
static void test_latest_independent(void)
{
    reset();c77_sample_t a,b;split_adc_snapshot stats;
    assert(!snapshot(0U,&a));advance(0U,7U,true);assert(!snapshot(0U,&a));
    advance(0U,1U,true);assert(snapshot(0U,&a));check_mean(0U,&a,8U);assert(copy_was_unmasked);
    assert(a.digital==5U&&a.encoders[1]==23U&&input_calls==1U);
    const uint32_t timestamp=a.timestamp_us;clock_us+=20U;
    assert(!snapshot(0U,&a)&&!a.valid&&banks[0].last_snapshot_sequence==8U);
    assert(!snapshot(1U,&b));advance(1U,8U,true);assert(snapshot(1U,&b));check_mean(1U,&b,8U);
    assert(!b.digital&&!b.encoders[0]&&input_calls==1U&&b.timestamp_us>timestamp);
    for(unsigned i=0;i<150U;i++) {
        advance(0U,1U,true);advance(1U,i%3U?1U:0U,true);
        assert(snapshot(0U,&a));check_mean(0U,&a,producer[0]);
        if(i%3U){assert(snapshot(1U,&b));check_mean(1U,&b,producer[1]);}
        else assert(!snapshot(1U,&b));
        adc_scan_stats(&stats);assert(stats.sample_seq==producer[1]);
        assert(!stats.errors&&scan_state.running);
    }
    assert(!adc_scan_snapshot(&a)&&!a.valid);
    assert(!adc_scan_bank_snapshot(2U,&a)&&!adc_scan_bank_snapshot(0U,NULL));
    assert(!basepri&&highest_mask==(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS)));
}
static void test_partial_and_pending(void)
{
    reset();advance(0U,15U,true);c77_sample_t out;assert(snapshot(0U,&out));
    /* ADC DMA has written only the first two ranks of frame16. */
    streams[0].NDTR-=2U;scan_buffer1.samples[15U*SCAN_ADC1_RANKS]=4000U;
    assert(!snapshot(0U,&out));advance(0U,1U,false);
    assert(snapshot(0U,&out));check_mean(0U,&out,16U);
    assert(banks[0].boundary_sequence==0U && banks[0].last_snapshot_sequence==16U);
    DMA2_Stream0_IRQHandler();assert(banks[0].boundary_sequence==16U && !snapshot(0U,&out));
    advance(0U,16U,false);streams[0].NDTR=0U;
    assert(snapshot(0U,&out));check_mean(0U,&out,32U);
    DMA2_Stream0_IRQHandler();assert(banks[0].boundary_sequence==32U);
    /* Caller masks are restored, rather than unconditionally enabled. */
    advance(0U,1U,true);basepri=48U;assert(snapshot(0U,&out));assert(basepri==48U);
}
static void test_copy_and_sum_races(void)
{
    c77_sample_t out;
    reset();advance(0U,15U,true);move_copy_frames=2U;copy_service=true;
    assert(snapshot(0U,&out));check_mean(0U,&out,15U);assert(banks[0].boundary_sequence==16U);
    assert(snapshot(0U,&out));check_mean(0U,&out,17U);
    reset();advance(0U,8U,true);move_copy_frames=24U;copy_service=true;
    assert(!snapshot(0U,&out)&&!banks[0].snapshot_seen&&scan_state.running);
    assert(snapshot(0U,&out));check_mean(0U,&out,32U);
    reset();advance(0U,8U,true);move_copy_frames=32U;copy_service=true;
    assert(!snapshot(0U,&out)&&!banks[0].snapshot_seen);
    reset();advance(0U,8U,true);copy_delay_cycles=ASYNC_COPY_MARGIN_CYCLES;
    assert(!snapshot(0U,&out)&&!banks[0].snapshot_seen);
    reset();advance(0U,8U,true);move_sum_frames=48U;
    assert(snapshot(0U,&out));check_mean(0U,&out,8U);
    assert(snapshot(0U,&out));check_mean(0U,&out,56U);
    reset();advance(0U,8U,true);stop_during_sum=true;
    assert(!snapshot(0U,&out)&&!banks[0].snapshot_seen&&!scan_state.running);
}
static void test_stale_faults_wrap_stop(void)
{
    c77_sample_t out;
    reset();advance(0U,8U,true);assert(snapshot(0U,&out));
    DWT->CYCCNT=ASYNC_RING_CYCLES;assert(!snapshot(0U,&out)&&!banks[1].snapshot_seen);
    reset();advance(0U,32U,false);DMA2_Stream0_IRQHandler();
    assert(!scan_state.running&&scan_state.stop_reason==SPLIT_ADC_STOP_FLAGS);
    reset();DMA2->LISR=DMA_LISR_TCIF1;DMA2_Stream1_IRQHandler();
    assert(!scan_state.running&&scan_state.stop_reason==SPLIT_ADC_STOP_FLAGS);
    reset();advance(0U,8U,true);scan_buffer1.samples[7U*SCAN_ADC1_RANKS]=4096U;
    assert(!snapshot(0U,&out)&&!banks[0].snapshot_seen&&scan_state.stop_reason==SPLIT_ADC_STOP_RAW_RANGE);
    reset();advance(0U,8U,true);DMA2->LISR|=DMA_LISR_TEIF1;
    assert(!snapshot(0U,&out)&&scan_state.dma_errors==1U&&!scan_state.running);
    reset();adc_registers[1].ISR=ADC_FLAG_OVR;ADC_IRQHandler();
    assert(scan_state.overruns==1U&&!scan_state.running&&scan_state.stop_reason==SPLIT_ADC_STOP_OVR);
    reset();advance(0U,8U,true);DMA2->LISR|=DMA_LISR_FEIF0;
    assert(snapshot(0U,&out));streams[0].FCR=DMA_SxFCR_FEIE;DMA2_Stream0_IRQHandler();
    assert(!scan_state.running&&scan_state.dma_errors==1U);
    reset();
    /* Monotonic per-bank sequence and clock both wrap naturally. */
    for(unsigned bank=0;bank<2U;bank++) {
        banks[bank].boundary_sequence=UINT32_MAX-15U;banks[bank].expected_half=1U;
        banks[bank].boundary_cycles=UINT32_MAX-1000U;banks[bank].history_ready=1U;
        producer[bank]=UINT32_MAX-15U;streams[bank].NDTR=16U*ranks_for(bank);
    }
    DWT->CYCCNT=UINT32_MAX-900U;advance(0U,16U,true);assert(!producer[0]);
    assert(snapshot(0U,&out));check_mean(0U,&out,0U);assert(!snapshot(0U,&out));
    last_stats_sequence=UINT32_MAX-15U;
    advance(1U,16U,true);split_adc_snapshot stats;adc_scan_stats(&stats);assert(!stats.sample_seq);
    assert(snapshot(1U,&out));check_mean(1U,&out,0U);
    dma_started=3U;adc_scan_stop();
    assert(stopped_adcs==3U&&disabled_nvic==7U&&cleared_nvic==7U&&timer_stop_calls==1U);
    assert(!snapshot(0U,&out));reset();advance(0U,7U,true);assert(!snapshot(0U,&out));
    advance(0U,1U,true);assert(snapshot(0U,&out));check_mean(0U,&out,8U);
}
#if !ROLE_LEFT
static uint16_t average_at(unsigned unit,uint32_t last,unsigned rank)
{
    uint32_t sum=0U;
    for(unsigned i=0;i<8U;i++)sum+=value_at(unit,last-i,rank);
    return (uint16_t)((sum+4U)/8U);
}
static uint16_t satellite_expected(unsigned unit,uint32_t last,unsigned key)
{
#if SCAN_ADC_PHASE_PAIR
    if(unit==0U||key!=0U){
        const unsigned rank=unit==0U?2U*key:2U*key-1U;
        return (uint16_t)((value_at(unit,last,rank)+value_at(unit,last,rank+1U)+1U)/2U);
    }
#endif
    return average_at(unit,last,key);
}
static void test_satellite_s7_publication(void)
{
    reset();c77_sample_t reply,decoded;uint8_t frame[32];
    assert(!adc_scan_snapshot(&reply));
    /* Bank0 must be retained when bank1 does not yet have eight samples. */
    advance(0U,10U,true);adc_scan_satellite_service();assert(!adc_scan_snapshot(&reply));
    advance(1U,8U,true);adc_scan_satellite_service();assert(adc_scan_snapshot(&reply));
    assert(reply.sequence==8U&&reply.digital==5U&&reply.encoders[0]==12U&&reply.encoders[1]==23U);
    for(unsigned rank=0;rank<4U;rank++)assert(reply.raw[rank]==satellite_expected(0U,10U,rank));
    for(unsigned rank=0;rank<3U;rank++)assert(reply.raw[rank+4U]==satellite_expected(1U,8U,rank));
    c77_split_reply(frame,28U,&reply);
    assert(c77_split_decode(frame,&decoded)&&decoded.valid&&decoded.sequence==8U&&c77_split_sequence(frame)==28U);
    assert(!memcmp(reply.raw,decoded.raw,sizeof reply.raw));
    advance(1U,1U,true);adc_scan_satellite_service();assert(adc_scan_snapshot(&reply));
    assert(reply.sequence==9U&&reply.raw[0]==satellite_expected(0U,10U,0U)&&reply.raw[4]==satellite_expected(1U,9U,0U));
    /* A newly advanced bank cannot refresh the other bank's age. */
    clock_us+=ASYNC_SATELLITE_MAX_AGE_US+1U;
    advance(0U,1U,true);adc_scan_satellite_service();assert(!adc_scan_snapshot(&reply));
    advance(1U,2U,true);adc_scan_satellite_service();assert(adc_scan_snapshot(&reply)&&reply.sequence==11U);
    adc_scan_stop();assert(!adc_scan_snapshot(&reply));
    reset();advance(1U,8U,true);adc_scan_satellite_service();assert(!adc_scan_snapshot(&reply));
}
#endif
int main(void)
{
    _Static_assert(SCAN_ADC_ASYNC && SCAN_ADC_EFFECTIVE_HZ==40000000U && SCAN_TARGET_HZ==EXPECTED_SCAN_HZ,"wrong test profile");
    _Static_assert(ASYNC_SCAN_CYCLES==(SCAN_TARGET_HZ==96000U?2500U:3750U),"wrong DWT scan period");
    _Static_assert(ASYNC_COPY_MARGIN_CYCLES==(SCAN_TARGET_HZ==96000U?60000U:90000U),"wrong overwrite-time bound");
    test_latest_independent();test_partial_and_pending();test_copy_and_sum_races();test_stale_faults_wrap_stop();
#if SCAN_ADC_PHASE_PAIR
    const uint8_t windows[]={1U,2U,4U,6U,8U};
    reset();advance(0U,8U,true);advance(1U,8U,true);
    c77_sample_t latest;
    assert(snapshot(0U,&latest)&&snapshot(1U,&latest));
#if !ROLE_LEFT
    advance(0U,1U,true);advance(1U,1U,true);adc_scan_satellite_service();
    assert(adc_scan_snapshot(&latest));
#endif
    set_scan_filter(4U);
#if !ROLE_LEFT
    assert(!adc_scan_snapshot(&latest));
#endif
    for(unsigned unit=0;unit<2U;unit++){
        assert(snapshot(unit,&latest));check_mean(unit,&latest,producer[unit]);
    }
    assert(!timer_stop_calls&&!stopped_adcs&&scan_state.running);
    for(unsigned n=0;n<sizeof windows;n++){
        reset();active_filter=windows[n];
        for(unsigned unit=0;unit<2U;unit++){
            c77_sample_t sample;advance(unit,17U,true);
            assert(snapshot(unit,&sample));check_mean(unit,&sample,17U);
        }
    }
#endif
#if !ROLE_LEFT
    test_satellite_s7_publication();
#endif
    printf("C86 async%uHz: independent latest8, partial ranks, pending boundaries, stale/overwrite rejection, wrap and error stop PASS\n",SCAN_TARGET_HZ);
    return 0;
}
