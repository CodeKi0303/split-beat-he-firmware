/* Explicit C86-left96k candidate. ADC1/2 are independent and share TIM3 TRGO.
 * This file is included by adc_scan.c only for SCAN_DUAL_96K. */
#include "hall_history.h"
_Static_assert(SCAN_ADC_DMA_FRAMES_PER_HALF==2U,"History batching requires two scans per DMA half");
static ADC_HandleTypeDef scan_adc[2];
static DMA_HandleTypeDef scan_dma[2];
static TIM_HandleTypeDef scan_timer;
static volatile union { uint16_t samples[2U*SCAN_ADC_DMA_FRAMES_PER_HALF*SCAN_ADC1_RANKS]; uint32_t words[SCAN_ADC_DMA_FRAMES_PER_HALF*SCAN_ADC1_RANKS]; }
    scan_buffer1 __attribute__((section(".dma_buffer"),aligned(32)));
static volatile union { uint16_t samples[2U*SCAN_ADC_DMA_FRAMES_PER_HALF*SCAN_ADC2_RANKS]; uint32_t words[SCAN_ADC_DMA_FRAMES_PER_HALF*SCAN_ADC2_RANKS]; }
    scan_buffer2 __attribute__((section(".dma_buffer"),aligned(32)));
static volatile split_adc_snapshot scan_state;
static volatile c77_sample_t input_frame;
static hall_history_t local_hall_history;
static uint32_t previous_publish_cycle,have_previous_publish;
static uint32_t dma_started,expected_half,pair_sequence;
static uint32_t scan_trigger_anchor;
static uint16_t pending_raw14[SCAN_ADC_DMA_FRAMES_PER_HALF][SPLIT_ADC_RANKS];

static void halt_from_irq(void)
{
    CLEAR_BIT(TIM3->CR1,TIM_CR1_CEN);
    for(unsigned unit=0;unit<2;unit++) {
        __HAL_ADC_DISABLE_IT(&scan_adc[unit],ADC_IT_OVR);
        DMA_Stream_TypeDef *stream=unit==0U?DMA2_Stream0:DMA2_Stream1;
        stream->CR&=~(DMA_SxCR_TCIE|DMA_SxCR_HTIE|DMA_SxCR_TEIE|DMA_SxCR_DMEIE);
        stream->FCR&=~DMA_SxFCR_FEIE;
        /* Non-blocking disable requests only. HAL Stop/Abort is main-only. */
        stream->CR&=~DMA_SxCR_EN;
    }
    scan_state.running=0U;
    hall_history_reset(&local_hall_history);
    __DSB();
}
static uint32_t trigger_cycle(void)
{
    return scan_trigger_anchor+(pair_sequence+SCAN_ADC_DMA_FRAMES_PER_HALF)*(C77_CPU_HZ/SCAN_TARGET_HZ);
}
static void capture_stop(uint32_t reason,uint32_t cycle)
{
    if(scan_state.stop_reason!=SPLIT_ADC_STOP_NONE) return;
    scan_state.stop_reason=reason;scan_state.stop_phase_cycles=cycle-trigger_cycle();
    scan_state.stop_dma_flags=DMA2->LISR;
    scan_state.stop_ndtr1=DMA2_Stream0->NDTR;scan_state.stop_ndtr2=DMA2_Stream1->NDTR;
}
static void late_dma(uint32_t reason,uint32_t cycle)
{
    capture_stop(reason,cycle);scan_state.late_irqs++;scan_state.errors++;halt_from_irq();
}
static unsigned ranks_for(unsigned unit) { return unit==0U?SCAN_ADC1_RANKS:SCAN_ADC2_RANKS; }
static int inactive_half(unsigned unit,uint32_t half,uint32_t remaining)
{
    const unsigned ranks=ranks_for(unit)*SCAN_ADC_DMA_FRAMES_PER_HALF;
    return half==0U ? remaining>=1U && remaining<=ranks
                    : remaining==0U || (remaining>ranks && remaining<=2U*ranks);
}
static uint32_t dma_fault_flags(unsigned unit,uint32_t flags)
{
    uint32_t mask=unit==0U?(DMA_LISR_TEIF0|DMA_LISR_DMEIF0):(DMA_LISR_TEIF1|DMA_LISR_DMEIF1);
    const DMA_Stream_TypeDef *stream=unit==0U?DMA2_Stream0:DMA2_Stream1;
    if(stream->FCR&DMA_SxFCR_FEIE)
        mask|=unit==0U?DMA_LISR_FEIF0:DMA_LISR_FEIF1;
    return flags&mask;
}
static int reject_pending_fault(void)
{
    /* A fault on the other ADC must prevent publication from this IRQ too. */
    for(unsigned unit=0;unit<2;unit++) {
        if(scan_adc[unit].Instance->ISR&ADC_FLAG_OVR) {
            scan_adc[unit].ErrorCode|=HAL_ADC_ERROR_OVR;
            HAL_ADC_ErrorCallback(&scan_adc[unit]);return 1;
        }
        const uint32_t flags=dma_fault_flags(unit,DMA2->LISR);
        if(flags) {
            if(flags&(unit==0U?DMA_LISR_TEIF0:DMA_LISR_TEIF1)) scan_dma[unit].ErrorCode|=HAL_DMA_ERROR_TE;
            if(flags&(unit==0U?DMA_LISR_DMEIF0:DMA_LISR_DMEIF1)) scan_dma[unit].ErrorCode|=HAL_DMA_ERROR_DME;
            if(flags&(unit==0U?DMA_LISR_FEIF0:DMA_LISR_FEIF1)) scan_dma[unit].ErrorCode|=HAL_DMA_ERROR_FE;
            scan_adc[unit].ErrorCode|=HAL_ADC_ERROR_DMA;
            scan_adc[unit].State|=HAL_ADC_STATE_ERROR_DMA;
            HAL_ADC_ErrorCallback(&scan_adc[unit]);return 1;
        }
    }
    return 0;
}
static void deliver_pair(uint32_t entered)
{
    if(!scan_state.running) return;
    /* Completion counts alone do not identify a common trigger: if one ADC
     * missed a conversion, its first HT could belong to the next TIM3 frame.
     * CPU/TIM3 are clock-locked at2:1; the left main loop does not enter WFI.
     * This epoch clock must not be reused on a sleeping satellite unchanged.
     * Use the IRQ entry timestamp: time spent checking/copying is not arrival
     * latency. Copy ownership is checked independently before and after copy.
     * Unsigned arithmetic remains valid across DWT/frame-count wrap. */
    /* A two-frame DMA half grants copying headroom, but accepting a whole
     * two-trigger epoch could pair ADC1 F1/F2 with ADC2 F2/F3 after a missed
     * trigger. Stop before the earliest ADC2 completion on the next trigger:
     * T +3*(32.5+7.5)/20MHz, with1us margin for anchor/read latency. */
    const uint32_t first_next_adc2=(C77_CPU_HZ/SCAN_TARGET_HZ)+
        (uint32_t)((uint64_t)C77_CPU_HZ*SCAN_ADC2_RANKS*
        (SCAN_ADC_SAMPLING_CYCLES_X2+SCAN_ADC_CONVERSION_CYCLES_X2)/(2ULL*SCAN_ADC_EFFECTIVE_HZ));
    const uint32_t deadline=first_next_adc2-C77_CPU_HZ/1000000U;
    if((uint32_t)(entered-trigger_cycle())>=deadline) {
        late_dma(SPLIT_ADC_STOP_EPOCH,entered);return;
    }
    const uint32_t half=expected_half;
    const uint32_t all_flags=DMA_LISR_HTIF0|DMA_LISR_TCIF0|DMA_LISR_HTIF1|DMA_LISR_TCIF1;
    const uint32_t wanted=half==0U?(DMA_LISR_HTIF0|DMA_LISR_HTIF1):(DMA_LISR_TCIF0|DMA_LISR_TCIF1);
    /* ADC2 has three ranks and must have completed before the four-rank ADC1.
     * HT/TC still latch even though ADC2 completion interrupts are disabled.
     * Missing/extra/mismatched flags reject skipped or mixed trigger frames. */
    if((DMA2->LISR&all_flags)!=wanted) {late_dma(SPLIT_ADC_STOP_FLAGS,entered);return;}
    if(!inactive_half(0U,half,DMA2_Stream0->NDTR) || !inactive_half(1U,half,DMA2_Stream1->NDTR)) {
        late_dma(SPLIT_ADC_STOP_OWNERSHIP,entered);return;
    }
    /* Prove both wanted flags were observed before a missing-trigger ADC2
     * could finish its shifted batch. Entry alone cannot prove that if work
     * before the register reads was delayed. Copy may cross this deadline,
     * provided the already-identified half remains DMA-inactive throughout. */
    const uint32_t observed=DWT->CYCCNT;
    if((uint32_t)(observed-trigger_cycle())>=deadline) {
        late_dma(SPLIT_ADC_STOP_EPOCH,observed);return;
    }
    DMA2->LIFCR=half==0U?(DMA_LIFCR_CHTIF0|DMA_LIFCR_CHTIF1):(DMA_LIFCR_CTCIF0|DMA_LIFCR_CTCIF1);
    __DMB();
    for(unsigned frame=0;frame<SCAN_ADC_DMA_FRAMES_PER_HALF;frame++) {
        const unsigned offset=half*SCAN_ADC_DMA_FRAMES_PER_HALF+frame;
        for(unsigned rank=0;rank<SCAN_ADC1_RANKS;rank++) pending_raw14[frame][rank]=scan_buffer1.samples[offset*SCAN_ADC1_RANKS+rank];
        for(unsigned rank=0;rank<SCAN_ADC2_RANKS;rank++) pending_raw14[frame][SCAN_ADC1_RANKS+rank]=scan_buffer2.samples[offset*SCAN_ADC2_RANKS+rank];
    }
    __DMB();
    if(!inactive_half(0U,half,DMA2_Stream0->NDTR) || !inactive_half(1U,half,DMA2_Stream1->NDTR)) {
        late_dma(SPLIT_ADC_STOP_OWNERSHIP,entered);return;
    }
    if(reject_pending_fault()) return;
    if(DMA2->LISR&all_flags) {late_dma(SPLIT_ADC_STOP_FLAGS,entered);return;}
    for(unsigned frame=0;frame<SCAN_ADC_DMA_FRAMES_PER_HALF;frame++)
    for(unsigned rank=0;rank<SPLIT_ADC_RANKS;rank++) if(pending_raw14[frame][rank]>16383U) {
        capture_stop(SPLIT_ADC_STOP_RAW_RANGE,entered);scan_state.errors++;halt_from_irq();
        return;
    }
    if(!hall_history_push2(&local_hall_history,pending_raw14)) {
        capture_stop(SPLIT_ADC_STOP_FILTER,entered);scan_state.errors++;halt_from_irq();return;
    }
    c77_sample_t next={0};
    /* Keep wire/calibration units12-bit.14-bit full scale16383 rounds4096,
     * so clamp explicitly. Decision Q5 keeps the full8-sample sum instead. */
    for(unsigned rank=0;rank<SPLIT_ADC_RANKS;rank++) {
        uint32_t value=((uint32_t)pending_raw14[SCAN_ADC_DMA_FRAMES_PER_HALF-1U][rank]+2U)>>2;
        next.raw[rank]=(uint16_t)(value>4095U?4095U:value);
    }
    const uint32_t cycle=DWT->CYCCNT;
    if(have_previous_publish) {
        const uint32_t elapsed=cycle-previous_publish_cycle;
        if(elapsed<scan_state.period_min_cycles) scan_state.period_min_cycles=elapsed;
        if(elapsed>scan_state.period_max_cycles) scan_state.period_max_cycles=elapsed;
    }
    previous_publish_cycle=cycle;have_previous_publish=1U;
    for(unsigned rank=0;rank<SPLIT_ADC_RANKS;rank++) scan_state.raw[rank]=next.raw[rank];
    scan_state.raw0=next.raw[0];scan_state.raw1=next.raw[1];
    /* Sequence counts physical scans96k/s; measured period bounds describe
     * publication batches48k/s. GPIO/quadrature are sampled once per batch. */
    scan_state.sample_cycles=cycle;scan_state.sample_seq+=SCAN_ADC_DMA_FRAMES_PER_HALF;
    next.sequence=scan_state.sample_seq;next.timestamp_us=board_micros();
    next.valid=1U;next.errors=scan_state.errors;
    board_input_scan(&next);input_frame=next;expected_half^=1U;pair_sequence+=SCAN_ADC_DMA_FRAMES_PER_HALF;__DMB();
}
void DMA2_Stream0_IRQHandler(void)
{
    const uint32_t entered=DWT->CYCCNT;
    if(scan_state.running && !reject_pending_fault()) deliver_pair(entered);
    const uint32_t elapsed=DWT->CYCCNT-entered;
    if(elapsed>scan_state.irq_max_cycles) scan_state.irq_max_cycles=elapsed;
    __DSB();
}
void DMA2_Stream1_IRQHandler(void)
{
    /* Error-only IRQ. Never clear ADC2 HT/TC here: ADC1 consumes the pair. */
    if(scan_state.running) (void)reject_pending_fault();
    __DSB();
}
void ADC_IRQHandler(void)
{
    HAL_ADC_IRQHandler(&scan_adc[0]);HAL_ADC_IRQHandler(&scan_adc[1]);__DSB();
}
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *adc)
{
    (void)adc; /* Completion publication is owned exclusively by Stream0 IRQ. */
}
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *adc)
{
    (void)adc;
}
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *adc)
{
    if(adc!=&scan_adc[0] && adc!=&scan_adc[1]) return;
    const uint32_t error=HAL_ADC_GetError(adc);scan_state.errors++;
    capture_stop((error&HAL_ADC_ERROR_OVR)?SPLIT_ADC_STOP_OVR:SPLIT_ADC_STOP_DMA,DWT->CYCCNT);
    if(error&HAL_ADC_ERROR_OVR) scan_state.overruns++;
    if(error&HAL_ADC_ERROR_DMA) scan_state.dma_errors++;
    halt_from_irq();
}
bool adc_scan_snapshot(c77_sample_t *out)
{
    if(out==NULL) return false;
    hall_history_t history;
    const uint32_t old=__get_BASEPRI();
    __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    *out=input_frame;history=local_hall_history;
    if(!scan_state.running || !out->sequence || board_micros()-out->timestamp_us>1000U) out->valid=0U;
    out->errors+=scan_state.errors;__set_BASEPRI(old);
    /* The IRQ only copied scans. Sum a coherent private eight-scan history
     * after releasing BASEPRI so UART, ADC and USB can run during arithmetic. */
    out->hall_q5_valid=(uint8_t)hall_history_q5(&history,out->hall_q5);
    if(!out->hall_q5_valid || !scan_state.running || board_micros()-out->timestamp_us>1000U)out->valid=0U;
    return out->valid!=0U;
}
void adc_scan_stats(split_adc_snapshot *out)
{
    if(out==NULL) return;
    const uint32_t old=__get_BASEPRI();
    __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    *out=scan_state;__set_BASEPRI(old);
}
void adc_scan_stop(void)
{
    if(scan_timer.Instance!=NULL) (void)HAL_TIM_Base_Stop(&scan_timer);
    HAL_NVIC_DisableIRQ(DMA2_Stream0_IRQn);HAL_NVIC_DisableIRQ(DMA2_Stream1_IRQn);HAL_NVIC_DisableIRQ(ADC_IRQn);
    scan_state.running=0U;hall_history_reset(&local_hall_history);
    for(unsigned unit=0;unit<2;unit++) if(dma_started&(1U<<unit)) (void)HAL_ADC_Stop_DMA(&scan_adc[unit]);
    dma_started=0U;
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);HAL_NVIC_ClearPendingIRQ(DMA2_Stream1_IRQn);HAL_NVIC_ClearPendingIRQ(ADC_IRQn);
}
static int init_failed(uint32_t reason)
{
    scan_state.init_error=reason;scan_state.errors++;adc_scan_stop();return 0;
}
bool adc_scan_start(void)
{
    memset((void*)&scan_state,0,sizeof scan_state);memset((void*)&input_frame,0,sizeof input_frame);
    memset(pending_raw14,0,sizeof pending_raw14);hall_history_reset(&local_hall_history);
    scan_state.period_min_cycles=UINT32_MAX;previous_publish_cycle=have_previous_publish=0U;
    expected_half=pair_sequence=dma_started=scan_trigger_anchor=0U;
    if(SystemCoreClock!=C77_CPU_HZ || C77_CPU_HZ!=240000000U ||
       HAL_RCC_GetSysClockFreq()!=240000000U || HAL_RCC_GetHCLKFreq()!=120000000U ||
       HAL_RCC_GetPCLK1Freq()!=60000000U || (RCC->CFGR&RCC_CFGR_TIMPRE)!=0U ||
       (RCC->D2CFGR&RCC_D2CFGR_D2PPRE1)!=RCC_D2CFGR_D2PPRE1_DIV2 ||
       HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_ADC)!=SCAN_ADC_KERNEL_HZ)
        return init_failed(SPLIT_ADC_INIT_CLOCK);
    const uintptr_t address1=(uintptr_t)&scan_buffer1,address2=(uintptr_t)&scan_buffer2;
    if(address1<0x24000000UL || address1+sizeof scan_buffer1>0x24050000UL || (address1&31U) ||
       address2<0x24000000UL || address2+sizeof scan_buffer2>0x24050000UL || (address2&31U) ||
       (SCB->CCR&SCB_CCR_DC_Msk)) return init_failed(SPLIT_ADC_INIT_MEMORY);
    CoreDebug->DEMCR|=CoreDebug_DEMCR_TRCENA_Msk;DWT->LAR=0xC5ACCE55U;
    if(DWT->CTRL&DWT_CTRL_NOCYCCNT_Msk) return init_failed(SPLIT_ADC_INIT_CYCLE_COUNTER);
    DWT->CTRL|=DWT_CTRL_CYCCNTENA_Msk;__DSB();__ISB();
    __HAL_RCC_GPIOA_CLK_ENABLE();__HAL_RCC_GPIOC_CLK_ENABLE();__HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_D2SRAM1_CLK_ENABLE();__HAL_RCC_D2SRAM2_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();__HAL_RCC_TIM3_CLK_ENABLE();
    for(unsigned i=0;i<SCAN_ADC_DMA_FRAMES_PER_HALF*SCAN_ADC1_RANKS;i++) scan_buffer1.words[i]=0U;
    for(unsigned i=0;i<SCAN_ADC_DMA_FRAMES_PER_HALF*SCAN_ADC2_RANKS;i++) scan_buffer2.words[i]=0U;
    __DMB();
    GPIO_InitTypeDef gpio={0};gpio.Mode=GPIO_MODE_ANALOG;gpio.Pull=GPIO_NOPULL;
    gpio.Pin=GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_4|GPIO_PIN_6;HAL_GPIO_Init(GPIOA,&gpio);
    gpio.Pin=GPIO_PIN_4|GPIO_PIN_5;HAL_GPIO_Init(GPIOC,&gpio);
    for(unsigned unit=0;unit<2;unit++) {
        scan_dma[unit].Instance=unit==0U?DMA2_Stream0:DMA2_Stream1;
        scan_dma[unit].Init.Request=unit==0U?DMA_REQUEST_ADC1:DMA_REQUEST_ADC2;
        scan_dma[unit].Init.Direction=DMA_PERIPH_TO_MEMORY;
        scan_dma[unit].Init.PeriphInc=DMA_PINC_DISABLE;scan_dma[unit].Init.MemInc=DMA_MINC_ENABLE;
        scan_dma[unit].Init.PeriphDataAlignment=DMA_PDATAALIGN_HALFWORD;
        scan_dma[unit].Init.MemDataAlignment=DMA_MDATAALIGN_HALFWORD;
        scan_dma[unit].Init.Mode=DMA_CIRCULAR;scan_dma[unit].Init.Priority=DMA_PRIORITY_VERY_HIGH;
        scan_dma[unit].Init.FIFOMode=DMA_FIFOMODE_DISABLE;
        if(HAL_DMA_Init(&scan_dma[unit])!=HAL_OK) return init_failed(SPLIT_ADC_INIT_DMA);
    }
    __HAL_LINKDMA(&scan_adc[0],DMA_Handle,scan_dma[0]);
    __HAL_LINKDMA(&scan_adc[1],DMA_Handle,scan_dma[1]);
    /* Timer initialization emits UG; finish it before either ADC is armed. */
    scan_timer.Instance=TIM3;scan_timer.Init.Prescaler=0U;scan_timer.Init.CounterMode=TIM_COUNTERMODE_UP;
    scan_timer.Init.Period=SCAN_TIMER_PERIOD;scan_timer.Init.ClockDivision=TIM_CLOCKDIVISION_DIV1;
    scan_timer.Init.AutoReloadPreload=TIM_AUTORELOAD_PRELOAD_DISABLE;
    if(HAL_TIM_Base_Init(&scan_timer)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_TIMER);
    TIM_MasterConfigTypeDef master={0};master.MasterOutputTrigger=TIM_TRGO_UPDATE;
    master.MasterSlaveMode=TIM_MASTERSLAVEMODE_DISABLE;
    if(HAL_TIMEx_MasterConfigSynchronization(&scan_timer,&master)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_TIMER);
    __HAL_TIM_SET_COUNTER(&scan_timer,0U);__HAL_TIM_CLEAR_FLAG(&scan_timer,TIM_FLAG_UPDATE);
    static const uint32_t ranks[4]={ADC_REGULAR_RANK_1,ADC_REGULAR_RANK_2,ADC_REGULAR_RANK_3,ADC_REGULAR_RANK_4};
    static const uint32_t channels[7]={ADC_CHANNEL_16,ADC_CHANNEL_17,ADC_CHANNEL_14,ADC_CHANNEL_4,
                                    ADC_CHANNEL_8,ADC_CHANNEL_18,ADC_CHANNEL_3};
    for(unsigned unit=0;unit<2;unit++) {
        ADC_HandleTypeDef *adc=&scan_adc[unit];adc->Instance=unit==0U?ADC1:ADC2;
        adc->Init.ClockPrescaler=ADC_CLOCK_ASYNC_DIV1;adc->Init.Resolution=ADC_RESOLUTION_14B;
        adc->Init.ScanConvMode=ADC_SCAN_ENABLE;adc->Init.EOCSelection=ADC_EOC_SEQ_CONV;
        adc->Init.LowPowerAutoWait=DISABLE;adc->Init.ContinuousConvMode=DISABLE;
        adc->Init.NbrOfConversion=ranks_for(unit);adc->Init.DiscontinuousConvMode=DISABLE;
        adc->Init.ExternalTrigConv=ADC_EXTERNALTRIG_T3_TRGO;adc->Init.ExternalTrigConvEdge=ADC_EXTERNALTRIGCONVEDGE_RISING;
        adc->Init.ConversionDataManagement=ADC_CONVERSIONDATA_DMA_CIRCULAR;adc->Init.Overrun=ADC_OVR_DATA_PRESERVED;
        adc->Init.LeftBitShift=ADC_LEFTBITSHIFT_NONE;adc->Init.OversamplingMode=DISABLE;
        if(HAL_ADC_Init(adc)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_ADC);
        if((adc->Instance->CR&ADC_CR_BOOST)!=ADC_CR_BOOST_1) return init_failed(SPLIT_ADC_INIT_CLOCK);
        ADC_ChannelConfTypeDef channel={0};channel.SamplingTime=ADC_SAMPLETIME_32CYCLES_5;
        channel.SingleDiff=ADC_SINGLE_ENDED;channel.OffsetNumber=ADC_OFFSET_NONE;
        for(unsigned rank=0;rank<ranks_for(unit);rank++) {
            channel.Channel=channels[(unit==0U?0U:SCAN_ADC1_RANKS)+rank];channel.Rank=ranks[rank];
            if(HAL_ADC_ConfigChannel(adc,&channel)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_CHANNEL);
        }
        if(HAL_ADCEx_Calibration_Start(adc,ADC_CALIB_OFFSET_LINEARITY,ADC_SINGLE_ENDED)!=HAL_OK)
            return init_failed(SPLIT_ADC_INIT_CALIBRATION);
    }
    ADC_MultiModeTypeDef mode={0};mode.Mode=ADC_MODE_INDEPENDENT;
    mode.DualModeData=ADC_DUALMODEDATAFORMAT_DISABLED;mode.TwoSamplingDelay=ADC_TWOSAMPLINGDELAY_1CYCLE;
    if(HAL_ADCEx_MultiModeConfigChannel(&scan_adc[0],&mode)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_ADC);
    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn,SCAN_ADC_IRQ_PRIORITY,0U);
    HAL_NVIC_SetPriority(DMA2_Stream1_IRQn,SCAN_ADC_IRQ_PRIORITY,0U);
    HAL_NVIC_SetPriority(ADC_IRQn,SCAN_ADC_IRQ_PRIORITY,0U);
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);HAL_NVIC_ClearPendingIRQ(DMA2_Stream1_IRQn);HAL_NVIC_ClearPendingIRQ(ADC_IRQn);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);HAL_NVIC_EnableIRQ(ADC_IRQn);
    /* Both armed before TIM3 starts: no initial offset or free-running ADC. */
    for(unsigned unit=0;unit<2;unit++) {
        uint32_t *buffer=(uint32_t*)(uintptr_t)(unit==0U?(uintptr_t)&scan_buffer1:(uintptr_t)&scan_buffer2);
        /* HAL may enable the ADC before a DMA-start failure. Include the
         * attempted unit in cleanup, not only units which returned HAL_OK. */
        dma_started|=1U<<unit;
        if(HAL_ADC_Start_DMA(&scan_adc[unit],buffer,2U*SCAN_ADC_DMA_FRAMES_PER_HALF*ranks_for(unit))!=HAL_OK) return init_failed(SPLIT_ADC_INIT_DMA_START);
    }
    /* ADC1's longer conversion drives one IRQ per two seven-key frames. Keep ADC2
     * error IRQs enabled, but consume its latched HT/TC flags from Stream0. */
    DMA2_Stream1->CR&=~(DMA_SxCR_HTIE|DMA_SxCR_TCIE);
    /* Publish the trigger-clock anchor before an ADC IRQ can use it. Reading
     * CNT immediately before DWT leaves only a small positive anchor skew;
     * no timer/ADC wait or polling is performed while interrupts are masked. */
    const uint32_t old_mask=__get_BASEPRI();
    __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    scan_state.running=1U;__DMB();
    const HAL_StatusTypeDef started=HAL_TIM_Base_Start(&scan_timer);
    const uint32_t timer_count=TIM3->CNT;
    scan_trigger_anchor=DWT->CYCCNT-timer_count*(C77_CPU_HZ/SCAN_TIMER_HZ);
    __DMB();__set_BASEPRI(old_mask);
    if(started!=HAL_OK) return init_failed(SPLIT_ADC_INIT_TIMER_START);
    return 1;
}
