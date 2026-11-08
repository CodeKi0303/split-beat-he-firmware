/* Production12-bit dual IRQ model; synthetic clocks do not measure MCU speed. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "adc_scan_private.h"
#include "adc_trace.h"
#include "hall_filter.h"
#define C77_CPU_HZ 240000000U
#define TEST_FRAMES SCAN_ADC_DMA_FRAMES_PER_HALF
#define TEST_PERIOD (C77_CPU_HZ/SCAN_TARGET_HZ)
#define TEST_BATCH_PERIOD (TEST_FRAMES*TEST_PERIOD)
#define TEST_BATCH_US ((TEST_BATCH_PERIOD+239U)/240U)
#define TEST_DEADLINE (SCAN_DUAL_12BIT_BATCHED?3964U:3750U)
typedef struct {uint32_t ISR;} ADC_TypeDef;
typedef struct {ADC_TypeDef *Instance;uint32_t ErrorCode,State;} ADC_HandleTypeDef;
typedef struct {uint32_t ErrorCode;} DMA_HandleTypeDef;
typedef struct {volatile uint32_t CR,FCR,NDTR;} DMA_Stream_TypeDef;
static ADC_TypeDef adc_registers[2];
static ADC_HandleTypeDef scan_adc[2];
static DMA_HandleTypeDef scan_dma[2];
static DMA_Stream_TypeDef streams[2];
#define DMA2_Stream0 (&streams[0])
#define DMA2_Stream1 (&streams[1])
static struct {uint32_t CR1;} timer_registers;
typedef struct {void *Instance;} TIM_HandleTypeDef;
static TIM_HandleTypeDef scan_timer;
static struct {uint32_t LISR,LIFCR;} dma_registers;
static struct {uint32_t CYCCNT;} cycle_registers;
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
static void clear_written_flags(void) {DMA2->LISR&=~DMA2->LIFCR;DMA2->LIFCR=0U;}
/* Simulate DMA changing at copy's second DMB; the direct NDTR reads in the
 * production code are unmodified. Also model processing across a trigger. */
static uint32_t barriers,mutate_unit,mutate_ndtr,mutate_flags,mutate_ovr,body_cycles;
static bool inspect_before_publish,frame_before_publish_valid;
static c77_sample_t frame_before_publish;
bool adc_scan_snapshot(c77_sample_t *out);
static void memory_barrier(void)
{
    clear_written_flags();barriers++;
    if(barriers==2U) {
        if(mutate_unit<2U)streams[mutate_unit].NDTR=mutate_ndtr;
        DMA2->LISR|=mutate_flags;
        if(mutate_ovr<2U)adc_registers[mutate_ovr].ISR|=ADC_FLAG_OVR;
        DWT->CYCCNT+=body_cycles;
    }
    if(barriers==3U && inspect_before_publish)
        frame_before_publish_valid=adc_scan_snapshot(&frame_before_publish);
}
#define __DSB() clear_written_flags()
#define __DMB() memory_barrier()
#define __ISB() ((void)0)
static uint32_t enabled_adc_irqs[2];
#define __HAL_ADC_DISABLE_IT(adc,bits) (enabled_adc_irqs[(adc)-scan_adc]&=~(bits))
static uint32_t basepri,highest_mask;
static uint32_t __get_BASEPRI(void) {return basepri;}
static void __set_BASEPRI(uint32_t mask) {basepri=mask;}
static void __set_BASEPRI_MAX(uint32_t mask) {if(!basepri || mask<basepri)basepri=mask;highest_mask=basepri;}
static volatile union {uint16_t samples[2U*TEST_FRAMES*SCAN_ADC1_RANKS];} scan_buffer1;
static volatile union {uint16_t samples[2U*TEST_FRAMES*SCAN_ADC2_RANKS];} scan_buffer2;
static volatile split_adc_snapshot scan_state;
static c77_sample_t input_frames[2];
static volatile uint32_t published_frame;
static hall_filter_t local_hall_filter;
static uint32_t previous_publish_cycle,have_previous_publish;
static uint32_t dma_started,expected_half,pair_sequence,scan_trigger_anchor;
static uint16_t pending_raw[TEST_FRAMES*7U];
static uint32_t clock_us,input_calls,input_error_total;
static bool inspect_during_write,frame_during_write_valid;
static c77_sample_t frame_during_write;
uint32_t board_micros(void) {return clock_us;}
static void board_input_scan(c77_sample_t *sample)
{
    if(inspect_during_write)frame_during_write_valid=adc_scan_snapshot(&frame_during_write);
    sample->digital=5;sample->encoders[0]=12;sample->encoders[1]=23;input_calls++;
    sample->errors+=input_error_total;
    DWT->CYCCNT+=37U; /* Synthetic observation, not measured MCU execution. */
}
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *adc);
static uint32_t HAL_ADC_GetError(ADC_HandleTypeDef *adc) {return adc->ErrorCode;}
static void HAL_ADC_IRQHandler(ADC_HandleTypeDef *adc)
{
    if(adc->Instance->ISR&ADC_FLAG_OVR) {adc->ErrorCode|=HAL_ADC_ERROR_OVR;HAL_ADC_ErrorCallback(adc);}
}
enum {DMA2_Stream0_IRQn=0,DMA2_Stream1_IRQn=1,ADC_IRQn=2};
static uint32_t stopped_adcs,disabled_nvic,cleared_nvic,timer_stop_calls;
static int HAL_TIM_Base_Stop(TIM_HandleTypeDef *timer)
{
    assert(timer==&scan_timer);TIM3->CR1=0U;timer_stop_calls++;return 0;
}
static int HAL_ADC_Stop_DMA(ADC_HandleTypeDef *adc) {stopped_adcs|=1U<<(unsigned)(adc-scan_adc);return 0;}
static void HAL_NVIC_DisableIRQ(unsigned irq) {disabled_nvic|=1U<<irq;}
static void HAL_NVIC_ClearPendingIRQ(unsigned irq) {cleared_nvic|=1U<<irq;}
#include "adc_dual64_under_test.inc"
static void reset(void)
{
    memset((void*)&scan_state,0,sizeof scan_state);memset(input_frames,0,sizeof input_frames);
    published_frame=0U;
    inspect_before_publish=frame_before_publish_valid=inspect_during_write=frame_during_write_valid=false;
    memset(pending_raw,0,sizeof pending_raw);hall_filter_reset(&local_hall_filter);
    scan_state.running=1;scan_state.period_min_cycles=UINT32_MAX;
    TIM3->CR1=1;DMA2->LISR=DMA2->LIFCR=0;scan_timer.Instance=&timer_registers;
    stopped_adcs=disabled_nvic=cleared_nvic=timer_stop_calls=dma_started=0;
    previous_publish_cycle=have_previous_publish=expected_half=pair_sequence=0;
    scan_trigger_anchor=0U-TEST_BATCH_PERIOD; /* First batch's last trigger is cycle0. */
    clock_us=input_calls=input_error_total=basepri=highest_mask=barriers=mutate_flags=body_cycles=0;
    mutate_unit=mutate_ovr=2U;mutate_ndtr=0;
    for(unsigned unit=0;unit<2;unit++) {
        scan_adc[unit].Instance=&adc_registers[unit];scan_adc[unit].ErrorCode=scan_adc[unit].State=0;
        scan_dma[unit].ErrorCode=adc_registers[unit].ISR=0;streams[unit].NDTR=2U*TEST_FRAMES*ranks_for(unit);
        streams[unit].CR=DMA_SxCR_EN|DMA_SxCR_TEIE|DMA_SxCR_DMEIE;
        if(unit==0U)streams[unit].CR|=DMA_SxCR_HTIE|DMA_SxCR_TCIE;
        streams[unit].FCR=0;enabled_adc_irqs[unit]=ADC_IT_OVR;
    }
}
static void fill_half(unsigned unit,unsigned half,uint16_t value)
{
    for(unsigned frame=0;frame<TEST_FRAMES;frame++)
    for(unsigned rank=0;rank<ranks_for(unit);rank++) {
        if(unit==0)scan_buffer1.samples[(half*TEST_FRAMES+frame)*SCAN_ADC1_RANKS+rank]=(uint16_t)(value+rank);
        else scan_buffer2.samples[(half*TEST_FRAMES+frame)*SCAN_ADC2_RANKS+rank]=(uint16_t)(value+rank);
    }
}
static void ready(unsigned unit,unsigned half)
{
    streams[unit].NDTR=(half?2U:1U)*TEST_FRAMES*ranks_for(unit);
    DMA2->LISR|=unit==0?(half?DMA_LISR_TCIF0:DMA_LISR_HTIF0):(half?DMA_LISR_TCIF1:DMA_LISR_HTIF1);
}
static void pair_ready(unsigned half) {fill_half(0,half,1000);fill_half(1,half,2000);ready(0,half);ready(1,half);}
static void service(uint32_t cycle) {DWT->CYCCNT=cycle;barriers=0;DMA2_Stream0_IRQHandler();}
static void stopped(unsigned sequence,unsigned reason)
{
    c77_sample_t sample={0};assert(!adc_scan_snapshot(&sample));
    assert(!scan_state.running && scan_state.sample_seq==sequence && scan_state.stop_reason==reason);
    assert(!(TIM3->CR1&TIM_CR1_CEN) && !local_hall_filter.ready);
    for(unsigned unit=0;unit<2;unit++)assert(!enabled_adc_irqs[unit] && !streams[unit].CR && !streams[unit].FCR);
}
static void test_single_boundary(void)
{
    for(unsigned order=0;order<2;order++) {
        reset();
        for(unsigned frame=0;frame<128;frame++) {
            const unsigned half=frame&1U;fill_half(0,half,1000);fill_half(1,half,2000);
            clock_us=frame*TEST_FRAMES*1000000U/SCAN_TARGET_HZ;
            ready(order,half);ready(order^1U,half);service(frame*TEST_BATCH_PERIOD+2400U);
            c77_sample_t sample={0};assert(adc_scan_snapshot(&sample));
            assert(sample.sequence==(frame+1U)*TEST_FRAMES && sample.valid && sample.hall_filtered_valid);
            for(unsigned i=0;i<7;i++) {
                const uint16_t raw=(uint16_t)(i<4?1000U+i:2000U+i-4U);
                assert(sample.raw[i]==raw && sample.hall_filtered[i]==raw && scan_state.raw[i]==sample.raw[i]);
            }
            assert(sample.digital==5 && sample.encoders[1]==23 && pair_sequence==(frame+1U)*TEST_FRAMES);
            assert(published_frame==((frame+1U)&1U));
        }
        assert(input_calls==128 && !scan_state.errors && scan_state.period_min_cycles==TEST_BATCH_PERIOD && scan_state.period_max_cycles==TEST_BATCH_PERIOD);
        assert(scan_state.irq_max_cycles==37 && basepri==0 && highest_mask==(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS)));
    }
    reset();ready(1,0);DWT->CYCCNT=1500;DMA2_Stream1_IRQHandler();
    assert(scan_state.running && !scan_state.sample_seq && (DMA2->LISR&DMA_LISR_HTIF1));
    fill_half(0,0,1000);fill_half(1,0,2000);ready(0,0);service(2400);assert(scan_state.sample_seq==TEST_FRAMES);
    /* Timely entry, then processing crosses next trigger while copy is safe. */
    reset();pair_ready(0);body_cycles=1600;service(2400);assert(scan_state.sample_seq==TEST_FRAMES && !scan_state.errors);
    assert(scan_state.irq_max_cycles==1637);
}
static void test_flags_epoch_ownership(void)
{
    reset();ready(0,0);service(2400);stopped(0,SPLIT_ADC_STOP_FLAGS); /* Missing ADC2 trigger. */
    assert(scan_state.stop_dma_flags==DMA_LISR_HTIF0 && scan_state.stop_ndtr1==4U*TEST_FRAMES && scan_state.stop_ndtr2==6U*TEST_FRAMES);
    reset();pair_ready(1);service(2400);stopped(0,SPLIT_ADC_STOP_FLAGS);
    reset();pair_ready(0);DMA2->LISR|=DMA_LISR_TCIF0;service(2400);stopped(0,SPLIT_ADC_STOP_FLAGS);
    reset();pair_ready(0);DMA2->LISR|=DMA_LISR_TCIF1;service(2400);stopped(0,SPLIT_ADC_STOP_FLAGS);
    reset();ready(0,0);ready(1,1);service(2400);stopped(0,SPLIT_ADC_STOP_FLAGS);
    reset();pair_ready(0);service(TEST_DEADLINE);stopped(0,SPLIT_ADC_STOP_EPOCH);assert(scan_state.stop_phase_cycles==TEST_DEADLINE);
    reset();pair_ready(0);service(TEST_DEADLINE-1U);assert(scan_state.sample_seq==TEST_FRAMES && !scan_state.errors);
    reset();pair_ready(0);service(4000);stopped(0,SPLIT_ADC_STOP_EPOCH); /* Same half, wrong trigger. */
    reset();pair_ready(0);DWT->CYCCNT=TEST_DEADLINE;deliver_pair(2400);stopped(0,SPLIT_ADC_STOP_EPOCH);
    assert(scan_state.stop_phase_cycles==TEST_DEADLINE); /* Timely entry, late flags observation. */
    reset();pair_ready(0);pair_sequence=5;service(2400);stopped(0,SPLIT_ADC_STOP_EPOCH);
    for(unsigned unit=0;unit<2;unit++) {
        reset();pair_ready(0);streams[unit].NDTR=2U*TEST_FRAMES*ranks_for(unit);service(2400);stopped(0,SPLIT_ADC_STOP_OWNERSHIP);
        reset();pair_ready(0);mutate_unit=unit;mutate_ndtr=2U*TEST_FRAMES*ranks_for(unit);service(2400);stopped(0,SPLIT_ADC_STOP_OWNERSHIP);
        assert((unit==0?scan_state.stop_ndtr1:scan_state.stop_ndtr2)==2U*TEST_FRAMES*ranks_for(unit));
        reset();pair_ready(0);mutate_flags=unit==0?DMA_LISR_TCIF0:DMA_LISR_TCIF1;service(2400);stopped(0,SPLIT_ADC_STOP_FLAGS);
    }
    reset();pair_ready(0);mutate_unit=1;mutate_ndtr=1;service(2400);assert(scan_state.sample_seq==TEST_FRAMES);
    reset();pair_ready(0);scan_trigger_anchor=UINT32_MAX-TEST_BATCH_PERIOD-99U;service(100);assert(scan_state.sample_seq==TEST_FRAMES);
    reset();pair_ready(0);pair_sequence=UINT32_MAX-(TEST_FRAMES-1U);scan_trigger_anchor=0;service(2400);
    assert(scan_state.sample_seq==TEST_FRAMES && !pair_sequence);
}
static void test_average_matches_installed_v036(void)
{
    uint16_t history[8][7]={0};unsigned slot=0;
    reset();
    for(unsigned batch=0;batch<64U;batch++) {
        const unsigned half=batch&1U;uint16_t raw[7];
        for(unsigned sub=0;sub<TEST_FRAMES;sub++) {
            const unsigned frame=batch*TEST_FRAMES+sub;
            for(unsigned key=0;key<7U;key++) {
                /* Different odd/even frames, ramp, and an impulse: skipping
                 * either frame in a batch cannot reproduce this average. */
                raw[key]=(uint16_t)(1000U+key*13U+frame*7U+(frame%2U?3U:0U)+(frame==19U?800U:0U));
                if(key<4U)scan_buffer1.samples[(half*TEST_FRAMES+sub)*4U+key]=raw[key];
                else scan_buffer2.samples[(half*TEST_FRAMES+sub)*3U+key-4U]=raw[key];
            }
            if(!frame)for(unsigned i=0;i<8U;i++)memcpy(history[i],raw,sizeof raw);
            else {memcpy(history[slot],raw,sizeof raw);slot=(slot+1U)%8U;}
        }
        ready(0,half);ready(1,half);service(batch*TEST_BATCH_PERIOD+2400U);
        c77_sample_t sample={0};assert(adc_scan_snapshot(&sample));
        assert(sample.sequence==(batch+1U)*TEST_FRAMES && sample.hall_filtered_valid);
        for(unsigned key=0;key<7U;key++) {
            uint32_t sum=0;for(unsigned i=0;i<8U;i++)sum+=history[i][key];
            assert(sample.raw[key]==raw[key] && sample.hall_filtered[key]==(sum+4U)/8U);
        }
    }
    assert(input_calls==64U && scan_state.sample_seq==64U*TEST_FRAMES);
}
static void test_slot_publication_is_coherent(void)
{
    reset();pair_ready(0);input_error_total=7U;service(2400U);
    c77_sample_t previous={0};assert(adc_scan_snapshot(&previous));
    assert(previous.sequence==TEST_FRAMES && previous.errors==7U);
    inspect_during_write=inspect_before_publish=true;
    fill_half(0,1,1200U);fill_half(1,1,2200U);ready(0,1);ready(1,1);
    input_error_total=9U;clock_us=TEST_BATCH_US;service(TEST_BATCH_PERIOD+2400U);
    assert(frame_during_write_valid && frame_before_publish_valid);
    assert(memcmp(&previous,&frame_during_write,sizeof previous)==0);
    assert(memcmp(&previous,&frame_before_publish,sizeof previous)==0);
    c77_sample_t current={0};assert(adc_scan_snapshot(&current));
    assert(current.sequence==2U*TEST_FRAMES && current.raw[0]==1200U && current.raw[4]==2200U);
    assert(current.timestamp_us==TEST_BATCH_US && current.hall_filtered[0]==1000U+25U*TEST_FRAMES && current.errors==9U);
    assert(published_frame==0U && pair_sequence==2U*TEST_FRAMES);
    /* Reuse the first slot without accumulating its previous input errors. */
    inspect_during_write=inspect_before_publish=false;
    pair_ready(0);input_error_total=11U;clock_us=2U*TEST_BATCH_US;service(2U*TEST_BATCH_PERIOD+2400U);
    assert(adc_scan_snapshot(&current));
    assert(published_frame==1U && current.sequence==3U*TEST_FRAMES && current.errors==11U);
    assert(current.raw[0]==1000U && current.timestamp_us==2U*TEST_BATCH_US);
}
static void test_abandoned_scan_does_not_publish(void)
{
    for(unsigned failure=0U;failure<3U;failure++) {
        reset();pair_ready(0);service(2400U);
        const uint32_t previous_slot=published_frame;
        const c77_sample_t previous=input_frames[previous_slot];
        fill_half(0,1,1500U);fill_half(1,1,2500U);ready(0,1);ready(1,1);
        if(failure==0U) {mutate_unit=1U;mutate_ndtr=TEST_FRAMES*SCAN_ADC2_RANKS;}
        else if(failure==1U)mutate_ovr=1U;
        else local_hall_filter.next=8U;
        clock_us=TEST_BATCH_US;service(TEST_BATCH_PERIOD+2400U);
        const unsigned reason=failure==0U?SPLIT_ADC_STOP_OWNERSHIP:
                              failure==1U?SPLIT_ADC_STOP_OVR:SPLIT_ADC_STOP_FILTER;
        stopped(TEST_FRAMES,reason);
        assert(published_frame==previous_slot && pair_sequence==TEST_FRAMES && input_calls==1U);
        assert(memcmp(&previous,&input_frames[previous_slot],sizeof previous)==0);
        assert(pending_raw[0]==1500U && pending_raw[4]==2500U);
        if(failure==2U) {
            /* Even an inactive slot partially filled before filter failure
             * stays private; the prior committed frame is never replaced. */
            assert(input_frames[previous_slot^1U].raw[0]==1500U);
            assert(input_frames[previous_slot^1U].sequence==0U);
        } else {
            /* DMA ownership/fault checks precede any mailbox mutation. */
            const c77_sample_t empty={0};
            assert(memcmp(&empty,&input_frames[previous_slot^1U],sizeof empty)==0);
        }
        c77_sample_t observed={0};assert(!adc_scan_snapshot(&observed));
        assert(observed.sequence==previous.sequence && observed.raw[0]==previous.raw[0]);
    }
}
static void test_errors_raw_filter(void)
{
    for(unsigned unit=0;unit<2;unit++) {
        reset();pair_ready(0);adc_registers[unit].ISR=ADC_FLAG_OVR;service(2400);stopped(0,SPLIT_ADC_STOP_OVR);
        assert(scan_state.overruns==1 && scan_state.errors==1);
        reset();pair_ready(0);DMA2->LISR|=unit==0?DMA_LISR_TEIF0:DMA_LISR_TEIF1;service(2400);stopped(0,SPLIT_ADC_STOP_DMA);
        assert(scan_state.dma_errors==1);
        reset();pair_ready(0);mutate_flags=unit==0?DMA_LISR_DMEIF0:DMA_LISR_DMEIF1;service(2400);stopped(0,SPLIT_ADC_STOP_DMA);
        reset();pair_ready(0);mutate_ovr=unit;service(2400);stopped(0,SPLIT_ADC_STOP_OVR);
        reset();streams[unit].FCR=DMA_SxFCR_FEIE;DMA2->LISR|=unit==0?DMA_LISR_FEIF0:DMA_LISR_FEIF1;
        DWT->CYCCNT=2400;DMA2_Stream1_IRQHandler();stopped(0,SPLIT_ADC_STOP_DMA);
    }
    reset();pair_ready(0);fill_half(0,0,4092);fill_half(1,0,4093);service(2400);
    c77_sample_t sample={0};assert(adc_scan_snapshot(&sample));
    assert(sample.raw[3]==4095U && sample.raw[6]==4095U && sample.hall_filtered[6]==4095U);
    clock_us=1000;assert(adc_scan_snapshot(&sample));clock_us=1001;assert(!adc_scan_snapshot(&sample));
    assert(!adc_scan_snapshot(NULL));split_adc_snapshot stats={0};adc_scan_stats(&stats);adc_scan_stats(NULL);
    assert(stats.sample_seq==TEST_FRAMES && stats.raw[6]==4095);
    for(unsigned unit=0;unit<2;unit++) {
        reset();pair_ready(0);fill_half(unit,0,4096);service(2400);stopped(0,SPLIT_ADC_STOP_RAW_RANGE);
    }
    reset();pair_ready(0);local_hall_filter.ready=1;local_hall_filter.next=8;service(2400);stopped(0,SPLIT_ADC_STOP_FILTER);
    reset();pair_ready(0);service(2400);adc_registers[1].ISR=ADC_FLAG_OVR;DWT->CYCCNT=TEST_BATCH_PERIOD+2100U;ADC_IRQHandler();
    stopped(TEST_FRAMES,SPLIT_ADC_STOP_OVR);assert(scan_state.stop_phase_cycles==2100);
    const uint32_t first_flags=scan_state.stop_dma_flags;
    DMA2->LISR|=DMA_LISR_TEIF1;HAL_ADC_ErrorCallback(&scan_adc[1]);
    assert(scan_state.stop_reason==SPLIT_ADC_STOP_OVR && scan_state.stop_dma_flags==first_flags);
}
static void test_stop_restart(void)
{
    reset();pair_ready(0);service(2400);ready(1,1);assert(local_hall_filter.ready);
    dma_started=3U;adc_scan_stop();assert(!scan_state.running && !local_hall_filter.ready && !dma_started);
    assert(stopped_adcs==3U && disabled_nvic==7U && cleared_nvic==7U && timer_stop_calls==1U);
    c77_sample_t sample={0};assert(!adc_scan_snapshot(&sample));
    reset();pair_ready(0);fill_half(0,0,3000);fill_half(1,0,3500);service(2400);
    assert(adc_scan_snapshot(&sample) && sample.hall_filtered[0]==3000U && sample.hall_filtered[4]==3500U);
}
static void test_two_scan_batch_boundaries(void)
{
#if SCAN_DUAL_12BIT_BATCHED
    /* Startup must seed from F1, then include F2, rather than seed from F2 or
     * skip F1. The published raw sample still belongs to the newest frame. */
    reset();pair_ready(0);
    for(unsigned key=0;key<7U;key++) {
        const uint16_t first=(uint16_t)(1000U+10U*key),last=(uint16_t)(first+80U);
        if(key<4U) {
            scan_buffer1.samples[key]=first;scan_buffer1.samples[4U+key]=last;
        } else {
            scan_buffer2.samples[key-4U]=first;scan_buffer2.samples[3U+key-4U]=last;
        }
    }
    service(2400U);c77_sample_t sample={0};assert(adc_scan_snapshot(&sample));
    for(unsigned key=0;key<7U;key++) {
        assert(sample.raw[key]==1080U+10U*key && sample.hall_filtered[key]==1010U+10U*key);
    }
    assert(sample.sequence==2U && input_calls==1U && local_hall_filter.next==1U);
    /* A late IRQ in the next trigger's acquisition phase can still own F1/F2;
     * a shifted ADC2 F2/F3 completion cannot pass the observed epoch bound. */
    reset();pair_ready(0);service(TEST_PERIOD+100U);assert(scan_state.sample_seq==2U);
    reset();pair_ready(0);DWT->CYCCNT=TEST_PERIOD+1704U;deliver_pair(2400U);
    stopped(0U,SPLIT_ADC_STOP_EPOCH);
    /* Invalid F1 must be rejected even when F2 is normal. Likewise F2 may
     * never contaminate history after a plausible first half of the batch. */
    for(unsigned sub=0;sub<2U;sub++) {
        reset();pair_ready(0);service(2400U);
        const uint32_t old_slot=published_frame;
        const c77_sample_t old=input_frames[old_slot];
        pair_ready(1);scan_buffer2.samples[(2U+sub)*3U]=4096U;
        service(TEST_BATCH_PERIOD+2400U);stopped(2U,SPLIT_ADC_STOP_RAW_RANGE);
        assert(published_frame==old_slot && pair_sequence==2U && input_calls==1U);
        assert(memcmp(&old,&input_frames[old_slot],sizeof old)==0);
    }
#endif
}
#if SCAN_ADC_CAPTURE_DIAG
#include "adc_trace_under_test.inc"
#include "adc_trace_checks.h"
static void trace_test_emit(unsigned frame)
{
    const unsigned half=frame&1U;
    for(unsigned key=0;key<7U;key++) {
        const uint16_t raw=trace_test_raw(frame,key);
        if(key<4U)scan_buffer1.samples[half*4U+key]=raw;
        else scan_buffer2.samples[half*3U+key-4U]=raw;
    }
    clock_us=frame*1000000U/SCAN_TARGET_HZ;
    ready(0U,half);ready(1U,half);service(frame*TEST_BATCH_PERIOD+2400U);
}
#endif
int main(void)
{
    assert(!SCAN_DUAL_96K && SCAN_TARGET_HZ==EXPECTED_SCAN_HZ && SCAN_ADC_EFFECTIVE_HZ==EXPECTED_ADC_HZ);
    assert(SCAN_ADC_BITS==12U && SCAN_ADC1_RANKS==4U && SCAN_ADC2_RANKS==3U);
    assert(sizeof(c77_sample_t)==48U);
#if C86_ADC_DUAL64_MATCHED40
    assert(SCAN_ADC_PLL2_P_DIV==6U && SCAN_ADC_EFFECTIVE_HZ==40000000U && SCAN_ADC_CAPTURE_DIAG);
#else
    assert(SCAN_ADC_PLL2_P_DIV==8U && SCAN_ADC_EFFECTIVE_HZ==30000000U);
#endif
#if SCAN_DUAL_12BIT_BATCHED
    assert(!SCAN_DUAL_64K && TEST_FRAMES==2U && SCAN_TARGET_HZ==96000U);
    assert(SCAN_PUBLICATION_HZ==48000U && SCAN_TIMER_PERIOD==1249U);
#else
    assert(SCAN_DUAL_64K && TEST_FRAMES==1U && SCAN_TARGET_HZ==64000U);
    assert(SCAN_PUBLICATION_HZ==64000U && SCAN_TIMER_PERIOD==1874U);
#endif
    assert(SCAN_ADC_SAMPLING_CYCLES_X2==129U && SCAN_ADC_CONVERSION_CYCLES_X2==13U);
    test_single_boundary();test_flags_epoch_ownership();test_average_matches_installed_v036();
    test_slot_publication_is_coherent();test_abandoned_scan_does_not_publish();
    test_errors_raw_filter();test_stop_restart();test_two_scan_batch_boundaries();
#if SCAN_ADC_CAPTURE_DIAG
    test_trace_snapshot();
#endif
    printf("C86 left%uHz12bit dual ADC (%u scans/half): paired publication,8tap mean, observed epoch/ownership/fault guards PASS\n",
        (unsigned)SCAN_TARGET_HZ,(unsigned)TEST_FRAMES);
    return 0;
}
