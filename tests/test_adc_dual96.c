/* Production96k IRQ test model; synthetic clocks do not measure MCU speed. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "adc_scan_private.h"
#include "hall_filter.h"
#include "hall_history.h"
#define C77_CPU_HZ 240000000U
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
static void memory_barrier(void)
{
    clear_written_flags();barriers++;
    if(barriers==2U) {
        if(mutate_unit<2U)streams[mutate_unit].NDTR=mutate_ndtr;
        DMA2->LISR|=mutate_flags;
        if(mutate_ovr<2U)adc_registers[mutate_ovr].ISR|=ADC_FLAG_OVR;
        DWT->CYCCNT+=body_cycles;
    }
}
#define __DSB() clear_written_flags()
#define __DMB() memory_barrier()
#define __ISB() ((void)0)
static uint32_t enabled_adc_irqs[2];
#define __HAL_ADC_DISABLE_IT(adc,bits) (enabled_adc_irqs[(adc)-scan_adc]&=~(bits))
static uint32_t basepri,highest_mask;
static unsigned mutate_on_unmask;
static void after_unmask(void);
static uint32_t __get_BASEPRI(void) {return basepri;}
static void __set_BASEPRI(uint32_t mask) {basepri=mask;if(!mask && mutate_on_unmask)after_unmask();}
static void __set_BASEPRI_MAX(uint32_t mask) {if(!basepri || mask<basepri)basepri=mask;highest_mask=basepri;}
static volatile union {uint16_t samples[2U*SCAN_ADC_DMA_FRAMES_PER_HALF*SCAN_ADC1_RANKS];} scan_buffer1;
static volatile union {uint16_t samples[2U*SCAN_ADC_DMA_FRAMES_PER_HALF*SCAN_ADC2_RANKS];} scan_buffer2;
static volatile split_adc_snapshot scan_state;
static volatile c77_sample_t input_frame;
static hall_history_t local_hall_history;
static uint32_t previous_publish_cycle,have_previous_publish;
static uint32_t dma_started,expected_half,pair_sequence,scan_trigger_anchor;
static uint16_t pending_raw14[SCAN_ADC_DMA_FRAMES_PER_HALF][7];
static uint32_t clock_us,input_calls;
uint32_t board_micros(void) {return clock_us;}
static void board_input_scan(c77_sample_t *sample)
{
    sample->digital=5;sample->encoders[0]=12;sample->encoders[1]=23;input_calls++;
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
static bool foreground_q5(const hall_history_t *history,uint32_t out[7])
{
    assert(basepri==0U);return hall_history_q5(history,out);
}
#define hall_history_q5 foreground_q5
#include "adc_dual96_under_test.inc"
#undef hall_history_q5
static void after_unmask(void)
{
    mutate_on_unmask=0;
    /* A newer IRQ may publish immediately after the critical copy. */
    for(unsigned slot=0;slot<8U;slot++)for(unsigned key=0;key<7U;key++)local_hall_history.samples[slot][key]=10000U;
    input_frame.sequence+=2U;input_frame.raw[0]=2500U;
}
static void reset(void)
{
    memset((void*)&scan_state,0,sizeof scan_state);memset((void*)&input_frame,0,sizeof input_frame);
    memset(pending_raw14,0,sizeof pending_raw14);hall_history_reset(&local_hall_history);
    scan_state.running=1;scan_state.period_min_cycles=UINT32_MAX;
    TIM3->CR1=1;DMA2->LISR=DMA2->LIFCR=0;scan_timer.Instance=&timer_registers;
    stopped_adcs=disabled_nvic=cleared_nvic=timer_stop_calls=dma_started=0;
    previous_publish_cycle=have_previous_publish=expected_half=pair_sequence=0;
    scan_trigger_anchor=0U-5000U; /* First batch last trigger is cycle0. */
    clock_us=input_calls=basepri=highest_mask=barriers=mutate_flags=body_cycles=mutate_on_unmask=0;
    mutate_unit=mutate_ovr=2U;mutate_ndtr=0;
    for(unsigned unit=0;unit<2;unit++) {
        scan_adc[unit].Instance=&adc_registers[unit];scan_adc[unit].ErrorCode=scan_adc[unit].State=0;
        scan_dma[unit].ErrorCode=adc_registers[unit].ISR=0;streams[unit].NDTR=4U*ranks_for(unit);
        streams[unit].CR=DMA_SxCR_EN|DMA_SxCR_TEIE|DMA_SxCR_DMEIE;
        if(unit==0U)streams[unit].CR|=DMA_SxCR_HTIE|DMA_SxCR_TCIE;
        streams[unit].FCR=0;enabled_adc_irqs[unit]=ADC_IT_OVR;
    }
}
static void fill_half(unsigned unit,unsigned half,uint16_t value)
{
    for(unsigned frame=0;frame<SCAN_ADC_DMA_FRAMES_PER_HALF;frame++)
    for(unsigned rank=0;rank<ranks_for(unit);rank++) {
        const unsigned offset=half*SCAN_ADC_DMA_FRAMES_PER_HALF+frame;
        if(unit==0)scan_buffer1.samples[offset*SCAN_ADC1_RANKS+rank]=(uint16_t)(value+rank);
        else scan_buffer2.samples[offset*SCAN_ADC2_RANKS+rank]=(uint16_t)(value+rank);
    }
}
static void ready(unsigned unit,unsigned half)
{
    streams[unit].NDTR=half?4U*ranks_for(unit):2U*ranks_for(unit);
    DMA2->LISR|=unit==0?(half?DMA_LISR_TCIF0:DMA_LISR_HTIF0):(half?DMA_LISR_TCIF1:DMA_LISR_HTIF1);
}
static void pair_ready(unsigned half) {fill_half(0,half,4000);fill_half(1,half,8000);ready(0,half);ready(1,half);}
static void service(uint32_t cycle) {DWT->CYCCNT=cycle;barriers=0;DMA2_Stream0_IRQHandler();}
static void stopped(unsigned sequence,unsigned reason)
{
    c77_sample_t sample={0};assert(!adc_scan_snapshot(&sample));
    assert(!scan_state.running && scan_state.sample_seq==sequence && scan_state.stop_reason==reason);
    assert(!(TIM3->CR1&TIM_CR1_CEN) && !local_hall_history.ready);
    for(unsigned unit=0;unit<2;unit++)assert(!enabled_adc_irqs[unit] && !streams[unit].CR && !streams[unit].FCR);
}
static void test_single_boundary(void)
{
    for(unsigned order=0;order<2;order++) {
        reset();
        for(unsigned frame=0;frame<128;frame++) {
            const unsigned half=frame&1U;fill_half(0,half,4000);fill_half(1,half,8000);
            clock_us=frame*1000000U/48000U;ready(order,half);ready(order^1U,half);service(frame*5000U+2000U);
            c77_sample_t sample={0};assert(adc_scan_snapshot(&sample));
            assert(sample.sequence==2U*(frame+1U) && sample.valid && sample.hall_q5_valid && !sample.hall_filtered_valid);
            for(unsigned i=0;i<7;i++) {
                const uint16_t raw14=(uint16_t)(i<4?4000U+i:8000U+i-4U);
                assert(sample.raw[i]==(raw14+2U)/4U && sample.hall_q5[i]==8U*raw14 && scan_state.raw[i]==sample.raw[i]);
            }
            assert(sample.digital==5 && sample.encoders[1]==23 && pair_sequence==2U*(frame+1U));
        }
        assert(input_calls==128 && !scan_state.errors && scan_state.period_min_cycles==5000U && scan_state.period_max_cycles==5000U);
        assert(scan_state.irq_max_cycles==37 && basepri==0 && highest_mask==(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS)));
    }
    reset();ready(1,0);DWT->CYCCNT=1500;DMA2_Stream1_IRQHandler();
    assert(scan_state.running && !scan_state.sample_seq && (DMA2->LISR&DMA_LISR_HTIF1));
    fill_half(0,0,4000);fill_half(1,0,8000);ready(0,0);service(2000);assert(scan_state.sample_seq==2);
    /* Timely entry, then processing crosses next trigger while copy is safe. */
    reset();pair_ready(0);body_cycles=900;service(2000);assert(scan_state.sample_seq==2 && !scan_state.errors);
    assert(scan_state.irq_max_cycles==937);
}
static void test_flags_epoch_ownership(void)
{
    reset();ready(0,0);service(2000);stopped(0,SPLIT_ADC_STOP_FLAGS); /* Missing ADC2 trigger. */
    assert(scan_state.stop_dma_flags==DMA_LISR_HTIF0 && scan_state.stop_ndtr1==8 && scan_state.stop_ndtr2==12);
    reset();pair_ready(1);service(2000);stopped(0,SPLIT_ADC_STOP_FLAGS);
    reset();pair_ready(0);DMA2->LISR|=DMA_LISR_TCIF0;service(2000);stopped(0,SPLIT_ADC_STOP_FLAGS);
    reset();pair_ready(0);DMA2->LISR|=DMA_LISR_TCIF1;service(2000);stopped(0,SPLIT_ADC_STOP_FLAGS);
    reset();ready(0,0);ready(1,1);service(2000);stopped(0,SPLIT_ADC_STOP_FLAGS);
    reset();pair_ready(0);service(3700);stopped(0,SPLIT_ADC_STOP_EPOCH);assert(scan_state.stop_phase_cycles==3700);
    reset();pair_ready(0);service(3699);assert(scan_state.sample_seq==2 && !scan_state.errors);
    reset();pair_ready(0);service(4000);stopped(0,SPLIT_ADC_STOP_EPOCH); /* Same half, wrong trigger. */
    /* Timely entry cannot bless ADC2's shifted F2/F3 block if its flag is
     * observed only after the next-trigger completion could have occurred. */
    reset();pair_ready(0);DWT->CYCCNT=4000;deliver_pair(2000);stopped(0,SPLIT_ADC_STOP_EPOCH);
    assert(scan_state.stop_phase_cycles==4000U);
    reset();pair_ready(0);pair_sequence=5;service(2000);stopped(0,SPLIT_ADC_STOP_EPOCH);
    for(unsigned unit=0;unit<2;unit++) {
        reset();pair_ready(0);streams[unit].NDTR=4U*ranks_for(unit);service(2000);stopped(0,SPLIT_ADC_STOP_OWNERSHIP);
        reset();pair_ready(0);mutate_unit=unit;mutate_ndtr=4U*ranks_for(unit);service(2000);stopped(0,SPLIT_ADC_STOP_OWNERSHIP);
        assert((unit==0?scan_state.stop_ndtr1:scan_state.stop_ndtr2)==4U*ranks_for(unit));
        reset();pair_ready(0);mutate_flags=unit==0?DMA_LISR_TCIF0:DMA_LISR_TCIF1;service(2000);stopped(0,SPLIT_ADC_STOP_FLAGS);
    }
    reset();pair_ready(0);mutate_unit=1;mutate_ndtr=1;service(2000);assert(scan_state.sample_seq==2);
    reset();pair_ready(0);scan_trigger_anchor=UINT32_MAX-5099U;service(100);assert(scan_state.sample_seq==2);
    reset();pair_ready(0);pair_sequence=UINT32_MAX-1U;scan_trigger_anchor=0;service(2000);assert(scan_state.sample_seq==2 && !pair_sequence);
}
static void test_full_history_and_snapshot_race(void)
{
    uint16_t reference[8][7]={0};unsigned inserted=0;
    reset();
    for(unsigned batch=0;batch<40U;batch++) {
        const unsigned half=batch&1U;
        for(unsigned frame=0;frame<2U;frame++) {
            uint16_t values[7];
            for(unsigned key=0;key<7U;key++) {
                /* Every scan differs, so skipping the odd scan cannot pass. */
                values[key]=(uint16_t)(3000U+batch*73U+frame*211U+key*29U);
                const unsigned offset=half*2U+frame;
                if(key<4U)scan_buffer1.samples[offset*4U+key]=values[key];
                else scan_buffer2.samples[offset*3U+key-4U]=values[key];
            }
            if(!inserted)for(unsigned slot=0;slot<8U;slot++)memcpy(reference[slot],values,sizeof values);
            else memcpy(reference[(inserted-1U)%8U],values,sizeof values);
            inserted++;
        }
        ready(0,half);ready(1,half);service(batch*5000U+2000U);
        c77_sample_t sample={0};assert(adc_scan_snapshot(&sample));
        for(unsigned key=0;key<7U;key++) {
            uint32_t sum=0;for(unsigned slot=0;slot<8U;slot++)sum+=reference[slot][key];
            assert(sample.hall_q5[key]==sum);
            assert(sample.raw[key]==(uint16_t)((3000U+batch*73U+211U+key*29U+2U)/4U));
        }
        assert(sample.sequence==inserted && input_calls==batch+1U);
    }
    reset();pair_ready(0);service(2000);mutate_on_unmask=1;
    c77_sample_t old={0};assert(adc_scan_snapshot(&old));
    assert(old.sequence==2U && old.raw[0]==1000U && old.hall_q5[0]==32000U);
    c77_sample_t newer={0};assert(adc_scan_snapshot(&newer));
    assert(newer.sequence==4U && newer.raw[0]==2500U && newer.hall_q5[0]==80000U);
    /* Corrupted history must not expose raw fallback as a valid Hall sample. */
    local_hall_history.samples[3][0]=16384U;
    assert(!adc_scan_snapshot(&newer) && !newer.hall_q5_valid);
}
static void test_errors_raw_filter(void)
{
    for(unsigned unit=0;unit<2;unit++) {
        reset();pair_ready(0);adc_registers[unit].ISR=ADC_FLAG_OVR;service(2000);stopped(0,SPLIT_ADC_STOP_OVR);
        assert(scan_state.overruns==1 && scan_state.errors==1);
        reset();pair_ready(0);DMA2->LISR|=unit==0?DMA_LISR_TEIF0:DMA_LISR_TEIF1;service(2000);stopped(0,SPLIT_ADC_STOP_DMA);
        assert(scan_state.dma_errors==1);
        reset();pair_ready(0);mutate_flags=unit==0?DMA_LISR_DMEIF0:DMA_LISR_DMEIF1;service(2000);stopped(0,SPLIT_ADC_STOP_DMA);
        reset();pair_ready(0);mutate_ovr=unit;service(2000);stopped(0,SPLIT_ADC_STOP_OVR);
        reset();streams[unit].FCR=DMA_SxFCR_FEIE;DMA2->LISR|=unit==0?DMA_LISR_FEIF0:DMA_LISR_FEIF1;
        DWT->CYCCNT=2000;DMA2_Stream1_IRQHandler();stopped(0,SPLIT_ADC_STOP_DMA);
    }
    reset();pair_ready(0);fill_half(0,0,16380);fill_half(1,0,16381);service(2000);
    c77_sample_t sample={0};assert(adc_scan_snapshot(&sample));
    assert(sample.raw[3]==4095U && sample.raw[6]==4095U && sample.hall_q5[6]==131064U);
    clock_us=1000;assert(adc_scan_snapshot(&sample));clock_us=1001;assert(!adc_scan_snapshot(&sample));
    assert(!adc_scan_snapshot(NULL));split_adc_snapshot stats={0};adc_scan_stats(&stats);adc_scan_stats(NULL);
    assert(stats.sample_seq==2 && stats.raw[6]==4095);
    for(unsigned unit=0;unit<2;unit++) {
        reset();pair_ready(0);fill_half(unit,0,16384);service(2000);stopped(0,SPLIT_ADC_STOP_RAW_RANGE);
    }
    reset();pair_ready(0);local_hall_history.ready=1;local_hall_history.next=8;service(2000);stopped(0,SPLIT_ADC_STOP_FILTER);
    reset();pair_ready(0);service(2000);adc_registers[1].ISR=ADC_FLAG_OVR;DWT->CYCCNT=7100;ADC_IRQHandler();
    stopped(2,SPLIT_ADC_STOP_OVR);assert(scan_state.stop_phase_cycles==2100);
    const uint32_t first_flags=scan_state.stop_dma_flags;
    DMA2->LISR|=DMA_LISR_TEIF1;HAL_ADC_ErrorCallback(&scan_adc[1]);
    assert(scan_state.stop_reason==SPLIT_ADC_STOP_OVR && scan_state.stop_dma_flags==first_flags);
}
static void test_stop_restart(void)
{
    reset();pair_ready(0);service(2000);ready(1,1);assert(local_hall_history.ready);
    dma_started=3U;adc_scan_stop();assert(!scan_state.running && !local_hall_history.ready && !dma_started);
    assert(stopped_adcs==3U && disabled_nvic==7U && cleared_nvic==7U && timer_stop_calls==1U);
    c77_sample_t sample={0};assert(!adc_scan_snapshot(&sample));
    reset();pair_ready(0);fill_half(0,0,12000);fill_half(1,0,13000);service(2000);
    assert(adc_scan_snapshot(&sample) && sample.hall_q5[0]==96000U && sample.hall_q5[4]==104000U);
}
int main(void)
{
    assert(SCAN_DUAL_96K && SCAN_TARGET_HZ==96000U && SCAN_ADC_EFFECTIVE_HZ==20000000U);
    assert(SCAN_ADC_BITS==14U && SCAN_ADC1_RANKS==4U && SCAN_ADC2_RANKS==3U && SCAN_TIMER_PERIOD==1249U);
    test_single_boundary();test_flags_epoch_ownership();test_full_history_and_snapshot_race();test_errors_raw_filter();test_stop_restart();
    puts("C86 left96k ADC: batch2 IRQ, paired flags/ownership, coherent8-scan foreground Q5, stop diagnostics PASS");
    return 0;
}
