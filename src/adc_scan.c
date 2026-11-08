#include "board_internal.h"
#include "adc_scan_private.h"
#include "adc_trace.h"
#if defined(C86_ADC_IRQ_PROBE)
#include "adc_irq_probe.h"
#endif
#if defined(C86_CPU_PROFILE)
#include "cpu_profile.h"
#endif
#include "hall_filter.h"
#include "stm32h7xx_hal.h"
#include <stddef.h>
#include <string.h>

#if SCAN_ADC_ASYNC
#include "adc_scan_async64.inc"
#elif SCAN_DUAL_64K || SCAN_DUAL_12BIT_BATCHED
#include "adc_scan_dual64.inc"
#elif SCAN_DUAL_96K
#include "adc_scan_dual96.inc"
#else
static ADC_HandleTypeDef scan_adc;
static DMA_HandleTypeDef scan_dma;
static TIM_HandleTypeDef scan_timer;

/* DMA2 cannot access DTCM. The section is NOLOAD, so initialize through whole
 * words after enabling AXI SRAM. D-cache is disabled for DMA coherency. */
static volatile union {
    uint16_t samples[SPLIT_ADC_RANKS*2U];
    uint32_t words[SPLIT_ADC_RANKS];
} scan_buffer __attribute__((section(".dma_buffer"),aligned(32)));
static volatile split_adc_snapshot scan_state;
#if HARDWARE_REVISION==86 && ROLE_LEFT
/* The ISR fills the inactive slot before making it visible to readers. */
static c77_sample_t input_frames[2];
static volatile uint32_t published_frame;
#else
static volatile c77_sample_t input_frame;
#endif
static uint32_t previous_publish_cycle,have_previous_publish;
static uint32_t dma_started,expected_half;
#if HARDWARE_REVISION==86 && ROLE_LEFT
/* Keep the same eight-sample filter at64k/75k and two at16k. */
#define LOCAL_HALL_FILTER_SAMPLES (SCAN_TARGET_HZ==16000U ? 2U : 8U)
static hall_filter_t local_hall_filter;
_Static_assert(LOCAL_HALL_FILTER_SAMPLES<=HALL_FILTER_MAX_SAMPLES,
               "Hall filter history is too short for scan profile");
#endif

static void halt_from_irq(void)
{
    /* No HAL stop/abort or timeout in an ISR. Stop further triggers and mask
     * this scanner's sources; at most the already active scan can finish. */
    CLEAR_BIT(TIM3->CR1,TIM_CR1_CEN);
    __HAL_ADC_DISABLE_IT(&scan_adc,ADC_IT_OVR);
    __HAL_DMA_DISABLE_IT(&scan_dma,DMA_IT_TC|DMA_IT_HT|DMA_IT_TE|DMA_IT_DME);
    /* FE enable is in FCR, so this HAL requires a separate operation. */
    __HAL_DMA_DISABLE_IT(&scan_dma,DMA_IT_FE);
    scan_state.running=0U;
#if HARDWARE_REVISION==86 && ROLE_LEFT
    hall_filter_reset(&local_hall_filter);
#endif
    __DSB();
}

static void late_dma(void)
{
    scan_state.late_irqs++;
    scan_state.errors++;
    halt_from_irq();
}

static int inactive_half(uint32_t half,uint32_t remaining)
{
    /* Each half is one seven-rank scan. HT means DMA owns the second half, TC
     * means it owns the first. Zero can occur briefly before circular reload. */
    return half==0U ? (remaining>=1U && remaining<=SPLIT_ADC_RANKS)
                    : (remaining==0U || (remaining>SPLIT_ADC_RANKS && remaining<=2U*SPLIT_ADC_RANKS));
}

static void deliver_scan(uint32_t half
#if defined(C86_ADC_IRQ_PROBE)
                         ,bool probe
#endif
                         )
{
    if(scan_state.running==0U) return;
    if(half!=expected_half || !inactive_half(half,__HAL_DMA_GET_COUNTER(&scan_dma))) {
        late_dma();
        return;
    }
#if HARDWARE_REVISION==86 && ROLE_LEFT
    const uint32_t slot=published_frame^1U;
    c77_sample_t *next=&input_frames[slot];
    uint16_t *raw=next->raw;
#else
    uint16_t raw[SPLIT_ADC_RANKS];
#endif
#if defined(C86_ADC_IRQ_PROBE)
    if(probe)adc_irq_probe_mark(1U);
#endif
    __DMB();
    for(unsigned rank=0U;rank<SPLIT_ADC_RANKS;rank++)
        raw[rank]=scan_buffer.samples[half*SPLIT_ADC_RANKS+rank];
    __DMB();
#if defined(C86_ADC_IRQ_PROBE)
    if(probe)adc_irq_probe_mark(2U);
#endif
    if(!inactive_half(half,__HAL_DMA_GET_COUNTER(&scan_dma))) {
        late_dma();
        return;
    }
    expected_half^=1U;
    uint32_t now=DWT->CYCCNT;
    if(have_previous_publish) {
        uint32_t elapsed=now-previous_publish_cycle;
        if(elapsed<scan_state.period_min_cycles) scan_state.period_min_cycles=elapsed;
        if(elapsed>scan_state.period_max_cycles) scan_state.period_max_cycles=elapsed;
    }
    previous_publish_cycle=now;
    have_previous_publish=1U;
    for(unsigned rank=0U;rank<SPLIT_ADC_RANKS;rank++) scan_state.raw[rank]=raw[rank];
    scan_state.raw0=raw[0];
    scan_state.raw1=raw[1];
    scan_state.sample_cycles=now;
    scan_state.sample_seq++;
#if !(HARDWARE_REVISION==86 && ROLE_LEFT)
    c77_sample_t staging={0};
    c77_sample_t *next=&staging;
    for(unsigned i=0;i<SPLIT_ADC_RANKS;i++) next->raw[i]=raw[i];
#endif
#if HARDWARE_REVISION==86 && ROLE_LEFT
#if defined(C86_ADC_IRQ_PROBE)
    if(probe)adc_irq_probe_mark(3U);
#endif
    next->hall_filtered_valid=(uint8_t)hall_filter_push(&local_hall_filter,raw,
        next->hall_filtered,LOCAL_HALL_FILTER_SAMPLES);
#if defined(C86_ADC_IRQ_PROBE)
    if(probe && next->hall_filtered_valid)adc_irq_probe_mark(4U);
#endif
    if(!next->hall_filtered_valid) {
        memset(next->hall_filtered,0,sizeof next->hall_filtered);
#if SCAN_ADC_CAPTURE_DIAG
        /* A later valid input re-seeds all filter slots. Refuse diagnostic
         * histories for this run rather than labeling seed copies as scans. */
        scan_state.errors++;
#endif
    }
#endif
    next->sequence=scan_state.sample_seq;next->timestamp_us=board_micros();
    next->valid=1;next->errors=scan_state.errors;
    board_input_scan(next);
#if HARDWARE_REVISION==86 && ROLE_LEFT
    __DMB();
    published_frame=slot;
#else
    input_frame=staging;
    __DMB();
#endif
}

void DMA2_Stream0_IRQHandler(void)
{
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_BEGIN(cpu_irq);
#endif
    uint32_t entered=DWT->CYCCNT;
#if defined(C86_ADC_IRQ_PROBE)
    const bool probe=adc_irq_probe_active && scan_state.running!=0U &&
                     adc_irq_probe_select(scan_state.sample_seq);
#endif
    if(scan_state.running!=0U) {
        uint32_t flags=DMA2->LISR;
        uint32_t error_flags=flags&(DMA_LISR_TEIF0|DMA_LISR_DMEIF0);
        if(__HAL_DMA_GET_IT_SOURCE(&scan_dma,DMA_IT_FE)!=0U)
            error_flags|=flags&DMA_LISR_FEIF0;
        if(error_flags!=0U) {
            /* HAL's DMA error handler can poll shutdown. Record the codes and
             * disable without waiting here, as in the proven fast_scan path. */
            if(error_flags&DMA_LISR_TEIF0) scan_dma.ErrorCode|=HAL_DMA_ERROR_TE;
            if(error_flags&DMA_LISR_DMEIF0) scan_dma.ErrorCode|=HAL_DMA_ERROR_DME;
            if(error_flags&DMA_LISR_FEIF0) scan_dma.ErrorCode|=HAL_DMA_ERROR_FE;
            scan_adc.ErrorCode|=HAL_ADC_ERROR_DMA;
            scan_adc.State|=HAL_ADC_STATE_ERROR_DMA;
            HAL_ADC_ErrorCallback(&scan_adc);
            __HAL_DMA_DISABLE(&scan_dma);
        } else if(scan_adc.Instance->ISR&ADC_FLAG_OVR) {
            HAL_ADC_IRQHandler(&scan_adc);
        } else if((flags&(DMA_LISR_HTIF0|DMA_LISR_TCIF0))==
                  (DMA_LISR_HTIF0|DMA_LISR_TCIF0)) {
            /* Both unserved halves: ownership/history is ambiguous. Never
             * publish overwritten or mixed data as a new valid sample. */
            late_dma();
        } else if(flags&DMA_LISR_HTIF0) {
            DMA2->LIFCR=DMA_LIFCR_CHTIF0;
            deliver_scan(0U
#if defined(C86_ADC_IRQ_PROBE)
                         ,probe
#endif
                         );
        } else if(flags&DMA_LISR_TCIF0) {
            DMA2->LIFCR=DMA_LIFCR_CTCIF0;
            deliver_scan(1U
#if defined(C86_ADC_IRQ_PROBE)
                         ,probe
#endif
                         );
        }
        /* Handle these circular HT/TC events directly. Even after checking
         * error flags above, a new TE could arrive before HAL_DMA_IRQHandler's
         * own status read and enter its polling abort path. Never call it here.
         * HAL ADC/DMA handles remain BUSY until the main-only stop operation. */
    }
    uint32_t elapsed=DWT->CYCCNT-entered;
    if(elapsed>scan_state.irq_max_cycles) scan_state.irq_max_cycles=elapsed;
    __DSB();
#if defined(C86_ADC_IRQ_PROBE)
    if(probe)adc_irq_probe_finish(entered,scan_state.sample_seq,DWT->CYCCNT);
#endif
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_IRQ_END(cpu_irq,CPU_PROF_IRQ_ADC);
#endif
}

void ADC_IRQHandler(void)
{
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_BEGIN(cpu_irq);
#endif
    HAL_ADC_IRQHandler(&scan_adc);
    __DSB();
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_IRQ_END(cpu_irq,CPU_PROF_IRQ_ADC);
#endif
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *adc)
{
    if(adc==&scan_adc) deliver_scan(0U
#if defined(C86_ADC_IRQ_PROBE)
                                  ,false
#endif
                                  );
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *adc)
{
    if(adc==&scan_adc) deliver_scan(1U
#if defined(C86_ADC_IRQ_PROBE)
                                  ,false
#endif
                                  );
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *adc)
{
    if(adc!=&scan_adc) return;
    uint32_t error=HAL_ADC_GetError(adc);
    scan_state.errors++;
    if(error&HAL_ADC_ERROR_OVR) scan_state.overruns++;
    if(error&HAL_ADC_ERROR_DMA) scan_state.dma_errors++;
    halt_from_irq();
}

bool adc_scan_snapshot(c77_sample_t *out)
{
    if(out==NULL) return false;
    uint32_t old=__get_BASEPRI();
    __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
#if HARDWARE_REVISION==86 && ROLE_LEFT
    *out=input_frames[published_frame];
#else
    *out=input_frame;
#endif
    if(!scan_state.running || !out->sequence || board_micros()-out->timestamp_us>1000U) out->valid=0;
    out->errors+=scan_state.errors;
    __set_BASEPRI(old);
    return out->valid!=0;
}

void adc_scan_stats(split_adc_snapshot *out)
{
    if(out==NULL) return;
    uint32_t old=__get_BASEPRI();
    __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    *out=scan_state;
    __set_BASEPRI(old);
}

void adc_scan_stop(void)
{
    if(scan_timer.Instance!=NULL) (void)HAL_TIM_Base_Stop(&scan_timer);
    HAL_NVIC_DisableIRQ(DMA2_Stream0_IRQn);
    HAL_NVIC_DisableIRQ(ADC_IRQn);
    scan_state.running=0U;
#if HARDWARE_REVISION==86 && ROLE_LEFT
    hall_filter_reset(&local_hall_filter);
#endif
    if(dma_started) {
        (void)HAL_ADC_Stop_DMA(&scan_adc);
        dma_started=0U;
    }
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);
    HAL_NVIC_ClearPendingIRQ(ADC_IRQn);
}

static int init_failed(uint32_t reason)
{
    scan_state.init_error=reason;
    scan_state.errors++;
    adc_scan_stop();
    return 0;
}

bool adc_scan_start(void)
{
    memset((void*)&scan_state,0,sizeof scan_state);
#if HARDWARE_REVISION==86 && ROLE_LEFT
    memset(input_frames,0,sizeof input_frames);
    published_frame=0U;
    hall_filter_reset(&local_hall_filter);
#else
    memset((void*)&input_frame,0,sizeof input_frame);
#endif
    scan_state.period_min_cycles=UINT32_MAX;
    previous_publish_cycle=0U; have_previous_publish=0U;
    expected_half=0U; dma_started=0U;
    /* TIMPRE=0/APB1 /2 gives TIM3=120MHz. Check the independently selected ADC
     * kernel rather than inferring its rate from CPU or APB clocks. */
    if(SystemCoreClock!=C77_CPU_HZ || C77_CPU_HZ!=240000000U ||
       HAL_RCC_GetSysClockFreq()!=240000000U || HAL_RCC_GetHCLKFreq()!=120000000U ||
       HAL_RCC_GetPCLK1Freq()!=60000000U || (RCC->CFGR&RCC_CFGR_TIMPRE)!=0U ||
       (RCC->D2CFGR&RCC_D2CFGR_D2PPRE1)!=RCC_D2CFGR_D2PPRE1_DIV2 ||
       HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_ADC)!=SCAN_ADC_KERNEL_HZ)
        return init_failed(SPLIT_ADC_INIT_CLOCK);
    uintptr_t address=(uintptr_t)&scan_buffer;
    if(address<0x24000000UL || address+sizeof scan_buffer>0x24050000UL ||
       (address&31U)!=0U || (SCB->CCR&SCB_CCR_DC_Msk)!=0U)
        return init_failed(SPLIT_ADC_INIT_MEMORY);

    /* Do not reset CYCCNT: the UART benchmark already uses the same clock. */
    CoreDebug->DEMCR|=CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR=0xC5ACCE55U;
    if(DWT->CTRL&DWT_CTRL_NOCYCCNT_Msk) return init_failed(SPLIT_ADC_INIT_CYCLE_COUNTER);
    DWT->CTRL|=DWT_CTRL_CYCCNTENA_Msk;
    __DSB(); __ISB();

    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOC_CLK_ENABLE(); __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_D2SRAM1_CLK_ENABLE(); __HAL_RCC_D2SRAM2_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    for(unsigned word=0U;word<SPLIT_ADC_RANKS;word++) scan_buffer.words[word]=0U;
    __DMB();
    GPIO_InitTypeDef gpio={0};
    gpio.Pin=GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_4|GPIO_PIN_6; gpio.Mode=GPIO_MODE_ANALOG; gpio.Pull=GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA,&gpio);
    gpio.Pin=GPIO_PIN_4|GPIO_PIN_5;HAL_GPIO_Init(GPIOC,&gpio);

    scan_dma.Instance=DMA2_Stream0;
    scan_dma.Init.Request=DMA_REQUEST_ADC1;
    scan_dma.Init.Direction=DMA_PERIPH_TO_MEMORY;
    scan_dma.Init.PeriphInc=DMA_PINC_DISABLE; scan_dma.Init.MemInc=DMA_MINC_ENABLE;
    scan_dma.Init.PeriphDataAlignment=DMA_PDATAALIGN_HALFWORD;
    scan_dma.Init.MemDataAlignment=DMA_MDATAALIGN_HALFWORD;
    scan_dma.Init.Mode=DMA_CIRCULAR; scan_dma.Init.Priority=DMA_PRIORITY_VERY_HIGH;
    scan_dma.Init.FIFOMode=DMA_FIFOMODE_DISABLE;
    if(HAL_DMA_Init(&scan_dma)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_DMA);
    __HAL_LINKDMA(&scan_adc,DMA_Handle,scan_dma);

    /* HAL TIM init emits UG. Configure the timer before arming ADC so that
     * this software update cannot become an extra conversion trigger. */
    scan_timer.Instance=TIM3;
    scan_timer.Init.Prescaler=0U; scan_timer.Init.CounterMode=TIM_COUNTERMODE_UP;
    scan_timer.Init.Period=SCAN_TIMER_PERIOD;
    scan_timer.Init.ClockDivision=TIM_CLOCKDIVISION_DIV1;
    scan_timer.Init.AutoReloadPreload=TIM_AUTORELOAD_PRELOAD_DISABLE;
    if(HAL_TIM_Base_Init(&scan_timer)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_TIMER);
    TIM_MasterConfigTypeDef master={0};
    master.MasterOutputTrigger=TIM_TRGO_UPDATE; master.MasterSlaveMode=TIM_MASTERSLAVEMODE_DISABLE;
    if(HAL_TIMEx_MasterConfigSynchronization(&scan_timer,&master)!=HAL_OK)
        return init_failed(SPLIT_ADC_INIT_TIMER);
    __HAL_TIM_SET_COUNTER(&scan_timer,0U); __HAL_TIM_CLEAR_FLAG(&scan_timer,TIM_FLAG_UPDATE);

    scan_adc.Instance=ADC1;
    /* H723 fixed /2 follows async /1. C86 64k/75k uses PLL2P80MHz ->40MHz:
     * seven (64.5+6.5)-cycle ranks plus trigger allowance take12.513us.
     * Legacy/recovery16k uses CLKP25MHz ->12.5MHz and39.76us (<62.5us).
     * At40MHz each acquisition is1.6125us; actual sensor settling/noise and
     * interrupt load must be checked on the board, not inferred from timing. */
    scan_adc.Init.ClockPrescaler=ADC_CLOCK_ASYNC_DIV1;
    scan_adc.Init.Resolution=ADC_RESOLUTION_12B; scan_adc.Init.ScanConvMode=ADC_SCAN_ENABLE;
    scan_adc.Init.EOCSelection=ADC_EOC_SEQ_CONV; scan_adc.Init.LowPowerAutoWait=DISABLE;
    scan_adc.Init.ContinuousConvMode=DISABLE; scan_adc.Init.NbrOfConversion=SPLIT_ADC_RANKS;
    scan_adc.Init.DiscontinuousConvMode=DISABLE;
    scan_adc.Init.ExternalTrigConv=ADC_EXTERNALTRIG_T3_TRGO;
    scan_adc.Init.ExternalTrigConvEdge=ADC_EXTERNALTRIGCONVEDGE_RISING;
    scan_adc.Init.ConversionDataManagement=ADC_CONVERSIONDATA_DMA_CIRCULAR;
    scan_adc.Init.Overrun=ADC_OVR_DATA_PRESERVED;
    scan_adc.Init.LeftBitShift=ADC_LEFTBITSHIFT_NONE; scan_adc.Init.OversamplingMode=DISABLE;
    if(HAL_ADC_Init(&scan_adc)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_ADC);
    /* Pinned HAL accounts for H723's fixed /2 in ADC_ConfigureBoostMode.
     * Refuse a mismatched mode rather than clocking the analog core too fast. */
    uint32_t expected_boost=SCAN_ADC_EFFECTIVE_HZ>25000000U ?
        (ADC_CR_BOOST_1|ADC_CR_BOOST_0) : ADC_CR_BOOST_0;
    if((ADC1->CR&ADC_CR_BOOST)!=expected_boost) return init_failed(SPLIT_ADC_INIT_CLOCK);
    ADC_ChannelConfTypeDef channel={0};
    channel.SamplingTime=ADC_SAMPLETIME_64CYCLES_5;
    channel.SingleDiff=ADC_SINGLE_ENDED; channel.OffsetNumber=ADC_OFFSET_NONE;
    static const uint32_t ranks[SPLIT_ADC_RANKS]={ADC_REGULAR_RANK_1,ADC_REGULAR_RANK_2,
        ADC_REGULAR_RANK_3,ADC_REGULAR_RANK_4,ADC_REGULAR_RANK_5,ADC_REGULAR_RANK_6,
        ADC_REGULAR_RANK_7};
    for(unsigned rank=0U;rank<SPLIT_ADC_RANKS;rank++) {
        static const uint32_t channels[SPLIT_ADC_RANKS]={ADC_CHANNEL_16,ADC_CHANNEL_17,
            ADC_CHANNEL_14,ADC_CHANNEL_4,ADC_CHANNEL_8,ADC_CHANNEL_18,ADC_CHANNEL_3};
        channel.Channel=channels[rank];
        channel.Rank=ranks[rank];
        if(HAL_ADC_ConfigChannel(&scan_adc,&channel)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_CHANNEL);
    }
    if(HAL_ADCEx_Calibration_Start(&scan_adc,ADC_CALIB_OFFSET_LINEARITY,ADC_SINGLE_ENDED)!=HAL_OK)
        return init_failed(SPLIT_ADC_INIT_CALIBRATION);

    /* At64k DMA must preempt the bounded priority2 split-link callbacks. */
    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn,SCAN_ADC_IRQ_PRIORITY,0U);
    HAL_NVIC_SetPriority(ADC_IRQn,SCAN_ADC_IRQ_PRIORITY,0U);
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn); HAL_NVIC_ClearPendingIRQ(ADC_IRQn);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn); HAL_NVIC_EnableIRQ(ADC_IRQn);
    if(HAL_ADC_Start_DMA(&scan_adc,(uint32_t*)(uintptr_t)&scan_buffer,2U*SPLIT_ADC_RANKS)!=HAL_OK)
        return init_failed(SPLIT_ADC_INIT_DMA_START);
    dma_started=1U; scan_state.running=1U; __DMB();
    if(HAL_TIM_Base_Start(&scan_timer)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_TIMER_START);
    return 1;
}
#endif /* scan implementation */

#if SCAN_ADC_CAPTURE_DIAG
bool adc_scan_trace_snapshot(c77_sample_t *out,adc_trace_history *history,
                             const uint8_t keys[ADC_TRACE_KEYS])
{
    if(keys[0]>=SPLIT_ADC_RANKS || keys[1]>=SPLIT_ADC_RANKS || keys[0]==keys[1])
        return false;
    const uint32_t old=__get_BASEPRI();
    __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    *out=input_frames[published_frame];
    const unsigned oldest=local_hall_filter.next;
    const bool valid=scan_state.running && !scan_state.errors && out->valid && out->sequence>=ADC_TRACE_HISTORY &&
        board_micros()-out->timestamp_us<=1000U && out->hall_filtered_valid &&
        local_hall_filter.ready==1U && oldest<ADC_TRACE_HISTORY;
    if(valid){
        for(unsigned k=0;k<ADC_TRACE_KEYS;k++)
            for(unsigned i=0;i<ADC_TRACE_HISTORY;i++)
                history->raw[k][i]=local_hall_filter.history[(oldest+i)&7U][keys[k]];
        out->errors+=scan_state.errors;
    }
    __set_BASEPRI(old);
    return valid;
}
#endif
