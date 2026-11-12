/* C86 independent latest-eight acquisition. The ADCs share
 * a trigger rate but neither completion nor sequence is paired across banks.
 * Only sixteen-scan DMA boundaries interrupt; all averaging is main-context. */
#include "adc_async.h"
_Static_assert(SCAN_ADC_ASYNC && (SCAN_TARGET_HZ==64000U || SCAN_TARGET_HZ==96000U) && SCAN_ADC_BITS==12U &&
               SCAN_ADC_EFFECTIVE_HZ==40000000U && SCAN_ADC1_RANKS==(SCAN_ADC_PHASE_PAIR ? 8U : 4U) &&
               SCAN_ADC2_RANKS==(SCAN_ADC_PHASE_PAIR ? 5U : 3U),
               "Async ADC requires a supported C86 matched40 rank layout");
_Static_assert(C77_CPU_HZ%SCAN_TARGET_HZ==0U,"Async DWT timing requires an exact scan period");
#define ASYNC_RING_SCANS 32U
#define ASYNC_HALF_SCANS 16U
#define ASYNC_AVERAGE_SCANS 8U
#define ASYNC_SCAN_CYCLES (C77_CPU_HZ/SCAN_TARGET_HZ)
#define ASYNC_RING_CYCLES (ASYNC_RING_SCANS*ASYNC_SCAN_CYCLES)
#define ASYNC_COPY_MARGIN_CYCLES ((ASYNC_RING_SCANS-ASYNC_AVERAGE_SCANS)*ASYNC_SCAN_CYCLES)
static ADC_HandleTypeDef scan_adc[2];
static DMA_HandleTypeDef scan_dma[2];
static TIM_HandleTypeDef scan_timer;
static volatile union {uint16_t samples[ASYNC_RING_SCANS*SCAN_ADC1_RANKS];uint32_t words[ASYNC_HALF_SCANS*SCAN_ADC1_RANKS];}
    scan_buffer1 __attribute__((section(".dma_buffer"),aligned(32)));
static volatile union {uint16_t samples[ASYNC_RING_SCANS*SCAN_ADC2_RANKS];uint32_t words[ASYNC_HALF_SCANS*SCAN_ADC2_RANKS];}
    scan_buffer2 __attribute__((section(".dma_buffer"),aligned(32)));
typedef struct {
    uint32_t boundary_sequence,boundary_cycles,expected_half;
    uint32_t last_snapshot_sequence,snapshot_seen,history_ready;
} async_bank_state;
static volatile async_bank_state banks[2];
static volatile split_adc_snapshot scan_state;
static uint32_t dma_started,previous_boundary_cycle,have_previous_boundary,last_stats_sequence;
#if SCAN_ADC_PHASE_PAIR
static uint32_t active_rate=SCAN_TARGET_HZ;
static uint32_t ring_cycles=ASYNC_RING_CYCLES,copy_margin_cycles=ASYNC_COPY_MARGIN_CYCLES;
#define ADC_DEFAULT_FILTER 1U
static uint8_t active_filter=ADC_DEFAULT_FILTER;
uint32_t adc_scan_rate(void) {return active_rate;}
uint8_t adc_scan_filter(void) {return active_filter;}
#else
#define ring_cycles ASYNC_RING_CYCLES
#define copy_margin_cycles ASYNC_COPY_MARGIN_CYCLES
#endif
#if !ROLE_LEFT
#define ASYNC_SATELLITE_MAX_AGE_US 1000U
static c77_sample_t satellite_slots[2];
static volatile unsigned satellite_active;
static uint32_t satellite_bank_sequence[2],satellite_bank_time[2];
static unsigned satellite_bank_seen;

static void satellite_reset(void)
{
    /* Acquisition is stopped during restart. Keep the old slot intact until
     * the ISR has been redirected to a known invalid one. */
    unsigned next=satellite_active^1U;
    memset(&satellite_slots[next],0,sizeof satellite_slots[next]);
    __DMB();satellite_active=next;
    satellite_bank_seen=0U;
}
#endif
#if SCAN_ADC_PHASE_PAIR
static void set_scan_filter(uint8_t filter)
{
    if(active_filter==filter)return;
    active_filter=filter;
    banks[0].snapshot_seen=banks[1].snapshot_seen=0U;
#if !ROLE_LEFT
    satellite_reset();
#endif
}
#endif

static unsigned ranks_for(unsigned unit) {return unit==0U?SCAN_ADC1_RANKS:SCAN_ADC2_RANKS;}
#if SCAN_ADC_PHASE_PAIR
typedef struct {uint8_t first,second;} adc_rank_pair;
/* Keep physical acquisition slots separate from logical calibration/STATUS
 * keys. Left interleaves ADC1 and separates Shift's pair; right keeps its
 * qualified adjacent pairs. Runtime rates/filters share these same maps. */
static const uint8_t adc_rank_keys[2][8]={
#if ROLE_LEFT
    {0U,1U,2U,3U,0U,1U,2U,3U},
    {0U,1U,2U,2U,1U,0U,0U,0U}
#else
    {0U,0U,1U,1U,2U,2U,3U,3U},
    {0U,1U,1U,2U,2U,0U,0U,0U}
#endif
};
static const adc_rank_pair adc_key_ranks[2][4]={
#if ROLE_LEFT
    {{0U,4U},{1U,5U},{2U,6U},{3U,7U}},
    {{0U,0U},{1U,4U},{2U,3U},{0U,0U}}
#else
    {{0U,1U},{2U,3U},{4U,5U},{6U,7U}},
    {{0U,0U},{1U,2U},{3U,4U},{0U,0U}}
#endif
};
#endif
static DMA_Stream_TypeDef *stream_for(unsigned unit) {return unit==0U?DMA2_Stream0:DMA2_Stream1;}
static uint32_t completion_mask(unsigned unit)
{return unit==0U?(DMA_LISR_HTIF0|DMA_LISR_TCIF0):(DMA_LISR_HTIF1|DMA_LISR_TCIF1);}
static uint32_t half_flag(unsigned unit,unsigned half)
{return unit==0U?(half?DMA_LISR_TCIF0:DMA_LISR_HTIF0):(half?DMA_LISR_TCIF1:DMA_LISR_HTIF1);}
static uint32_t earlier_sequence(uint32_t a,uint32_t b)
{return (int32_t)(a-b)<=0?a:b;}
static void halt_from_irq(void)
{
    CLEAR_BIT(TIM3->CR1,TIM_CR1_CEN);
    for(unsigned unit=0;unit<2U;unit++) {
        __HAL_ADC_DISABLE_IT(&scan_adc[unit],ADC_IT_OVR);
        DMA_Stream_TypeDef *stream=stream_for(unit);
        stream->CR&=~(DMA_SxCR_TCIE|DMA_SxCR_HTIE|DMA_SxCR_TEIE|DMA_SxCR_DMEIE|DMA_SxCR_EN);
        stream->FCR&=~DMA_SxFCR_FEIE;
    }
    scan_state.running=0U;__DSB();
}
static void capture_stop(uint32_t reason,uint32_t cycle)
{
    if(scan_state.stop_reason!=SPLIT_ADC_STOP_NONE)return;
    scan_state.stop_reason=reason;
    /* Async has no common frame epoch: phase is age of the last slower-bank
     * serviced boundary. The diagnostics metadata must preserve that meaning. */
    const unsigned slow=(int32_t)(banks[0].boundary_sequence-banks[1].boundary_sequence)<=0?0U:1U;
    scan_state.stop_phase_cycles=cycle-banks[slow].boundary_cycles;
    scan_state.stop_dma_flags=DMA2->LISR;
    scan_state.stop_ndtr1=DMA2_Stream0->NDTR;scan_state.stop_ndtr2=DMA2_Stream1->NDTR;
}
static void late_dma(uint32_t reason,uint32_t cycle)
{
    capture_stop(reason,cycle);scan_state.errors++;scan_state.late_irqs++;halt_from_irq();
}
static uint32_t dma_fault_flags(unsigned unit,uint32_t flags)
{
    uint32_t mask=unit==0U?(DMA_LISR_TEIF0|DMA_LISR_DMEIF0):(DMA_LISR_TEIF1|DMA_LISR_DMEIF1);
    if(stream_for(unit)->FCR&DMA_SxFCR_FEIE)mask|=unit==0U?DMA_LISR_FEIF0:DMA_LISR_FEIF1;
    return flags&mask;
}
static int reject_pending_fault(void)
{
    const uint32_t flags=DMA2->LISR;
    for(unsigned unit=0;unit<2U;unit++) {
        if(scan_adc[unit].Instance->ISR&ADC_FLAG_OVR) {
            scan_adc[unit].ErrorCode|=HAL_ADC_ERROR_OVR;HAL_ADC_ErrorCallback(&scan_adc[unit]);return 1;
        }
        const uint32_t fault=dma_fault_flags(unit,flags);
        if(fault) {
            if(fault&(unit?DMA_LISR_TEIF1:DMA_LISR_TEIF0))scan_dma[unit].ErrorCode|=HAL_DMA_ERROR_TE;
            if(fault&(unit?DMA_LISR_DMEIF1:DMA_LISR_DMEIF0))scan_dma[unit].ErrorCode|=HAL_DMA_ERROR_DME;
            if(fault&(unit?DMA_LISR_FEIF1:DMA_LISR_FEIF0))scan_dma[unit].ErrorCode|=HAL_DMA_ERROR_FE;
            scan_adc[unit].ErrorCode|=HAL_ADC_ERROR_DMA;scan_adc[unit].State|=HAL_ADC_STATE_ERROR_DMA;
            HAL_ADC_ErrorCallback(&scan_adc[unit]);return 1;
        }
    }
    return 0;
}
/* Called with ADC IRQs masked or in a same-priority ADC handler. No loop/wait:
 * a boundary arriving between register reads is a retryable snapshot miss.
 * Pending HT/TC extends the serviced boundary exactly once without consuming
 * it. A full lap cannot alias because both flags or elapsed-time guards fail. */
static bool completed_cursor(unsigned unit,uint32_t cycle,uint32_t *sequence)
{
    const uint32_t mask=completion_mask(unit),flags_before=DMA2->LISR&mask;
    const uint32_t remaining=stream_for(unit)->NDTR;
    const uint32_t flags_after=DMA2->LISR&mask;
    if(flags_before!=flags_after || remaining>ASYNC_RING_SCANS*ranks_for(unit))return false;
    if(cycle-banks[unit].boundary_cycles>=ring_cycles)return false;
    uint32_t base=banks[unit].boundary_sequence;
    if(flags_after) {
        if(flags_after!=half_flag(unit,banks[unit].expected_half))return false;
        base+=ASYNC_HALF_SCANS;
    }
    const uint32_t position=((ASYNC_RING_SCANS*ranks_for(unit)-remaining)/ranks_for(unit))&(ASYNC_RING_SCANS-1U);
    const uint32_t since=(position-(base&(ASYNC_RING_SCANS-1U)))&(ASYNC_RING_SCANS-1U);
    if(since>=ASYNC_HALF_SCANS)return false;
    *sequence=base+since;return true;
}
static void boundary_irq(unsigned unit)
{
    const uint32_t entered=DWT->CYCCNT;
    if(scan_state.running && !reject_pending_fault()) {
        const uint32_t flags=DMA2->LISR&completion_mask(unit);
        if(flags) {
            const uint32_t wanted=half_flag(unit,banks[unit].expected_half);
            uint32_t cursor;
            if(flags!=wanted)late_dma(SPLIT_ADC_STOP_FLAGS,entered);
            else if(!completed_cursor(unit,entered,&cursor))late_dma(SPLIT_ADC_STOP_OWNERSHIP,entered);
            else {
                DMA2->LIFCR=unit==0U?(banks[unit].expected_half?DMA_LIFCR_CTCIF0:DMA_LIFCR_CHTIF0):
                                             (banks[unit].expected_half?DMA_LIFCR_CTCIF1:DMA_LIFCR_CHTIF1);
                banks[unit].boundary_sequence+=ASYNC_HALF_SCANS;
                banks[unit].expected_half^=1U;banks[unit].boundary_cycles=entered;
                banks[unit].history_ready=1U;
                scan_state.sample_seq=earlier_sequence(banks[0].boundary_sequence,banks[1].boundary_sequence);
                scan_state.sample_cycles=entered;
                if(unit==0U) {
                    if(have_previous_boundary) {
                        const uint32_t period=entered-previous_boundary_cycle;
                        if(period<scan_state.period_min_cycles)scan_state.period_min_cycles=period;
                        if(period>scan_state.period_max_cycles)scan_state.period_max_cycles=period;
                    }
                    previous_boundary_cycle=entered;have_previous_boundary=1U;
                }
            }
        }
    }
    const uint32_t elapsed=DWT->CYCCNT-entered;
    if(elapsed>scan_state.irq_max_cycles)scan_state.irq_max_cycles=elapsed;
    __DSB();
}
void DMA2_Stream0_IRQHandler(void) {boundary_irq(0U);}
void DMA2_Stream1_IRQHandler(void) {boundary_irq(1U);}
void ADC_IRQHandler(void)
{HAL_ADC_IRQHandler(&scan_adc[0]);HAL_ADC_IRQHandler(&scan_adc[1]);__DSB();}
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *adc) {(void)adc;}
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *adc) {(void)adc;}
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *adc)
{
    if(adc!=&scan_adc[0] && adc!=&scan_adc[1])return;
    const uint32_t error=HAL_ADC_GetError(adc);scan_state.errors++;
    capture_stop((error&HAL_ADC_ERROR_OVR)?SPLIT_ADC_STOP_OVR:SPLIT_ADC_STOP_DMA,DWT->CYCCNT);
    if(error&HAL_ADC_ERROR_OVR)scan_state.overruns++;
    if(error&HAL_ADC_ERROR_DMA)scan_state.dma_errors++;
    halt_from_irq();
}
bool adc_scan_bank_snapshot(unsigned unit,c77_sample_t *out)
{
    if(out==NULL || unit>1U)return false;
    out->valid=0U;
    enum { MAX_RANKS = SCAN_ADC1_RANKS > SCAN_ADC2_RANKS ? SCAN_ADC1_RANKS : SCAN_ADC2_RANKS };
    uint16_t history[ASYNC_AVERAGE_SCANS][MAX_RANKS];
    uint32_t rank_sums[MAX_RANKS];
    const unsigned ranks=ranks_for(unit),first=unit==0U?0U:SCAN_ADC1_KEYS;
    const unsigned key_count=unit==0U?SCAN_ADC1_KEYS:SCAN_ADC_RANKS-SCAN_ADC1_KEYS;
    const unsigned latest=ASYNC_AVERAGE_SCANS-1U;
#if SCAN_ADC_PHASE_PAIR
    const unsigned frames=active_filter;
#else
    const unsigned frames=ASYNC_AVERAGE_SCANS;
#endif
    const unsigned first_frame=ASYNC_AVERAGE_SCANS-frames;
    const uint32_t old=__get_BASEPRI();
    __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    const uint32_t started=DWT->CYCCNT;
    uint32_t sequence,after;
    if(!scan_state.running || reject_pending_fault() || !completed_cursor(unit,started,&sequence) ||
       (!banks[unit].history_ready && sequence<ASYNC_AVERAGE_SCANS) ||
       (banks[unit].snapshot_seen && sequence==banks[unit].last_snapshot_sequence)) {
        __set_BASEPRI(old);return false;
    }
    __set_BASEPRI(old);
    const volatile uint16_t *ring=unit==0U?scan_buffer1.samples:scan_buffer2.samples;
    __DMB();
    for(unsigned i=first_frame;i<ASYNC_AVERAGE_SCANS;i++) {
        const unsigned position=(sequence-ASYNC_AVERAGE_SCANS+i)&(ASYNC_RING_SCANS-1U);
        for(unsigned rank=0;rank<ranks;rank++)history[i][rank]=ring[position*ranks+rank];
    }
    __DMB();
    const uint32_t post_mask=__get_BASEPRI();
    __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    const uint32_t finished=DWT->CYCCNT;
    const bool copy_ok=scan_state.running && !reject_pending_fault() &&
        completed_cursor(unit,finished,&after) &&
        after-sequence<ASYNC_RING_SCANS-ASYNC_AVERAGE_SCANS &&
        finished-started<copy_margin_cycles;
    const uint32_t observed_us=board_micros();
    __set_BASEPRI(post_mask);
    if(!copy_ok)return false;
    c77_sample_t sample={0};
    for(unsigned rank=0;rank<ranks;rank++) {
        uint32_t sum=0U;
        for(unsigned i=first_frame;i<ASYNC_AVERAGE_SCANS;i++) {
            if(history[i][rank]>4095U) {
                const uint32_t mask=__get_BASEPRI();
                __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
                capture_stop(SPLIT_ADC_STOP_RAW_RANGE,DWT->CYCCNT);scan_state.errors++;halt_from_irq();
                __set_BASEPRI(mask);return false;
            }
            sum+=history[i][rank];
        }
        rank_sums[rank]=sum;
    }
    for(unsigned key=0;key<key_count;key++) {
#if SCAN_ADC_PHASE_PAIR
        if(unit==0U || key!=0U) {
            const adc_rank_pair pair=adc_key_ranks[unit][key];
            sample.raw[first+key]=(uint16_t)((history[latest][pair.first]+history[latest][pair.second]+1U)/2U);
            sample.hall_filtered[first+key]=(uint16_t)((rank_sums[pair.first]+rank_sums[pair.second]+frames)/(2U*frames));
            continue;
        }
#endif
        sample.raw[first+key]=history[latest][key];
        sample.hall_filtered[first+key]=(uint16_t)((rank_sums[key]+frames/2U)/frames);
    }
    sample.sequence=sequence;sample.timestamp_us=observed_us;
    sample.valid=sample.hall_filtered_valid=1U;
    if(unit==0U)board_input_scan(&sample);
    /* Sum and GPIO work run with IRQs restored. A stop during that work must
     * not publish data whose acquisition is no longer healthy. */
    const uint32_t final_mask=__get_BASEPRI();
    __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    if(!scan_state.running) {__set_BASEPRI(final_mask);return false;}
    banks[unit].last_snapshot_sequence=sequence;banks[unit].snapshot_seen=1U;banks[unit].history_ready=1U;
    for(unsigned key=0;key<key_count;key++)scan_state.raw[first+key]=sample.raw[first+key];
    scan_state.raw0=scan_state.raw[0];scan_state.raw1=scan_state.raw[1];
    sample.errors+=scan_state.errors;*out=sample;
    __set_BASEPRI(final_mask);return true;
}
bool adc_scan_snapshot(c77_sample_t *out)
{
#if ROLE_LEFT
    /* The main board consumes the two banks individually. */
    if(out)memset(out,0,sizeof *out);
    return false;
#else
    if(out==NULL)return false;
    const unsigned index=satellite_active;
    __DMB();*out=satellite_slots[index];
    out->errors|=scan_state.errors;
    if(!scan_state.running || !out->valid ||
       (uint32_t)(board_micros()-out->timestamp_us)>ASYNC_SATELLITE_MAX_AGE_US)
        out->valid=0U;
    return out->valid!=0U;
#endif
}
#if !ROLE_LEFT
void adc_scan_satellite_service(void)
{
    const unsigned current=satellite_active;
    c77_sample_t combined=satellite_slots[current];
    bool advanced=false;
    for(unsigned unit=0;unit<2U;unit++) {
        c77_sample_t bank;
        if(!adc_scan_bank_snapshot(unit,&bank))continue;
        advanced=true;satellite_bank_seen|=1U<<unit;
        satellite_bank_sequence[unit]=bank.sequence;
        satellite_bank_time[unit]=bank.timestamp_us;
        const unsigned first=unit==0U?0U:SCAN_ADC1_KEYS;
        const unsigned end=unit==0U?SCAN_ADC1_KEYS:SCAN_ADC_RANKS;
        for(unsigned i=first;i<end;i++)
            combined.raw[i]=bank.hall_filtered[i]; /* Existing S7 Hall fields. */
        if(unit==0U) {
            combined.digital=bank.digital;
            combined.encoders[0]=bank.encoders[0];combined.encoders[1]=bank.encoders[1];
            combined.errors=bank.errors;
        }
    }
    if(!advanced)return;
    combined.valid=(uint8_t)(satellite_bank_seen==3U);
    if(combined.valid) {
        combined.sequence=earlier_sequence(satellite_bank_sequence[0],satellite_bank_sequence[1]);
        combined.timestamp_us=earlier_sequence(satellite_bank_time[0],satellite_bank_time[1]);
    }
    /* The IRQ can interrupt the copy, but always reads only the active slot.
     * No averaging, loop or interrupt mask in the S7 reply path. */
    const unsigned next=current^1U;
    satellite_slots[next]=combined;
    __DMB();satellite_active=next;
}
#endif
void adc_scan_stats(split_adc_snapshot *out)
{
    if(out==NULL)return;
    const uint32_t old=__get_BASEPRI();
    __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    *out=scan_state;
    uint32_t a,b;
    const uint32_t now=DWT->CYCCNT;
    uint32_t latest=scan_state.sample_seq;
    if(scan_state.running && completed_cursor(0U,now,&a) && completed_cursor(1U,now,&b))
        latest=earlier_sequence(a,b);
    /* A transient cursor retry must not make rate counters move backwards. */
    if((int32_t)(latest-last_stats_sequence)>0)last_stats_sequence=latest;
    out->sample_seq=last_stats_sequence;
    __set_BASEPRI(old);
}
void adc_scan_stop(void)
{
    if(scan_timer.Instance!=NULL)(void)HAL_TIM_Base_Stop(&scan_timer);
    HAL_NVIC_DisableIRQ(DMA2_Stream0_IRQn);HAL_NVIC_DisableIRQ(DMA2_Stream1_IRQn);HAL_NVIC_DisableIRQ(ADC_IRQn);
    scan_state.running=0U;
    for(unsigned unit=0;unit<2U;unit++)if(dma_started&(1U<<unit))(void)HAL_ADC_Stop_DMA(&scan_adc[unit]);
    dma_started=0U;
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);HAL_NVIC_ClearPendingIRQ(DMA2_Stream1_IRQn);HAL_NVIC_ClearPendingIRQ(ADC_IRQn);
}
static int init_failed(uint32_t reason)
{
    scan_state.init_error=reason;scan_state.errors++;adc_scan_stop();return 0;
}
#if SCAN_ADC_PHASE_PAIR
static bool adc_configured;
#endif
static void reset_scan_state(void)
{
    memset((void*)&scan_state,0,sizeof scan_state);memset((void*)banks,0,sizeof banks);
#if !ROLE_LEFT
    satellite_reset();
#endif
    scan_state.period_min_cycles=UINT32_MAX;
    dma_started=previous_boundary_cycle=have_previous_boundary=last_stats_sequence=0U;
}
static bool arm_scan(void)
{
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);HAL_NVIC_ClearPendingIRQ(DMA2_Stream1_IRQn);HAL_NVIC_ClearPendingIRQ(ADC_IRQn);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);HAL_NVIC_EnableIRQ(ADC_IRQn);
    /* Both armed before TIM3 starts: no initial offset or free-running ADC. */
    for(unsigned unit=0;unit<2;unit++) {
        uint32_t *buffer=(uint32_t*)(uintptr_t)(unit==0U?(uintptr_t)&scan_buffer1:(uintptr_t)&scan_buffer2);
        /* HAL may enable the ADC before a DMA-start failure. Include the
         * attempted unit in cleanup, not only units which returned HAL_OK. */
        dma_started|=1U<<unit;
        if(HAL_ADC_Start_DMA(&scan_adc[unit],buffer,ASYNC_RING_SCANS*ranks_for(unit))!=HAL_OK)
            return init_failed(SPLIT_ADC_INIT_DMA_START);
    }
    const uint32_t old_mask=__get_BASEPRI();
    __set_BASEPRI_MAX(SCAN_ADC_IRQ_PRIORITY<<(8U-__NVIC_PRIO_BITS));__DSB();__ISB();
    scan_state.running=1U;__DMB();
    const HAL_StatusTypeDef started=HAL_TIM_Base_Start(&scan_timer);
    const uint32_t started_cycle=DWT->CYCCNT;
    banks[0].boundary_cycles=banks[1].boundary_cycles=started_cycle;
    __DMB();__set_BASEPRI(old_mask);
    if(started!=HAL_OK) return init_failed(SPLIT_ADC_INIT_TIMER_START);
    return true;
}
bool adc_scan_start(void)
{
    reset_scan_state();
#if SCAN_ADC_PHASE_PAIR
    adc_configured=false;
#endif
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
    for(unsigned i=0;i<ASYNC_HALF_SCANS*SCAN_ADC1_RANKS;i++) scan_buffer1.words[i]=0U;
    for(unsigned i=0;i<ASYNC_HALF_SCANS*SCAN_ADC2_RANKS;i++) scan_buffer2.words[i]=0U;
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
#if SCAN_ADC_PHASE_PAIR
    scan_timer.Init.Period=SCAN_TIMER_HZ/active_rate-1U;
#else
    scan_timer.Init.Period=SCAN_TIMER_PERIOD;
#endif
    scan_timer.Init.ClockDivision=TIM_CLOCKDIVISION_DIV1;
    scan_timer.Init.AutoReloadPreload=TIM_AUTORELOAD_PRELOAD_DISABLE;
    if(HAL_TIM_Base_Init(&scan_timer)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_TIMER);
    TIM_MasterConfigTypeDef master={0};master.MasterOutputTrigger=TIM_TRGO_UPDATE;
    master.MasterSlaveMode=TIM_MASTERSLAVEMODE_DISABLE;
    if(HAL_TIMEx_MasterConfigSynchronization(&scan_timer,&master)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_TIMER);
    __HAL_TIM_SET_COUNTER(&scan_timer,0U);__HAL_TIM_CLEAR_FLAG(&scan_timer,TIM_FLAG_UPDATE);
    static const uint32_t ranks[]={ADC_REGULAR_RANK_1,ADC_REGULAR_RANK_2,ADC_REGULAR_RANK_3,ADC_REGULAR_RANK_4,
                                  ADC_REGULAR_RANK_5,ADC_REGULAR_RANK_6,ADC_REGULAR_RANK_7,ADC_REGULAR_RANK_8};
    static const uint32_t channels[]={ADC_CHANNEL_16,ADC_CHANNEL_17,ADC_CHANNEL_14,ADC_CHANNEL_4,
                                     ADC_CHANNEL_8,ADC_CHANNEL_18,ADC_CHANNEL_3};
    for(unsigned unit=0;unit<2;unit++) {
        ADC_HandleTypeDef *adc=&scan_adc[unit];adc->Instance=unit==0U?ADC1:ADC2;
        adc->Init.ClockPrescaler=ADC_CLOCK_ASYNC_DIV1;adc->Init.Resolution=ADC_RESOLUTION_12B;
        adc->Init.ScanConvMode=ADC_SCAN_ENABLE;adc->Init.EOCSelection=ADC_EOC_SEQ_CONV;
        adc->Init.LowPowerAutoWait=DISABLE;adc->Init.ContinuousConvMode=DISABLE;
        adc->Init.NbrOfConversion=ranks_for(unit);adc->Init.DiscontinuousConvMode=DISABLE;
        adc->Init.ExternalTrigConv=ADC_EXTERNALTRIG_T3_TRGO;adc->Init.ExternalTrigConvEdge=ADC_EXTERNALTRIGCONVEDGE_RISING;
        adc->Init.ConversionDataManagement=ADC_CONVERSIONDATA_DMA_CIRCULAR;adc->Init.Overrun=ADC_OVR_DATA_PRESERVED;
        adc->Init.LeftBitShift=ADC_LEFTBITSHIFT_NONE;adc->Init.OversamplingMode=DISABLE;
        if(HAL_ADC_Init(adc)!=HAL_OK) return init_failed(SPLIT_ADC_INIT_ADC);
        if((adc->Instance->CR&ADC_CR_BOOST)!=(ADC_CR_BOOST_1|ADC_CR_BOOST_0)) return init_failed(SPLIT_ADC_INIT_CLOCK);
        ADC_ChannelConfTypeDef channel={0};
        channel.SingleDiff=ADC_SINGLE_ENDED;channel.OffsetNumber=ADC_OFFSET_NONE;
        for(unsigned rank=0;rank<ranks_for(unit);rank++) {
            const unsigned first=unit==0U?0U:SCAN_ADC1_KEYS;
#if SCAN_ADC_PHASE_PAIR
            const unsigned key=adc_rank_keys[unit][rank];
#else
            const unsigned key=rank;
#endif
            channel.Channel=channels[first+key];channel.Rank=ranks[rank];
            channel.SamplingTime=SCAN_ADC_PHASE_PAIR && (unit==0U || rank!=0U)?
                ADC_SAMPLETIME_32CYCLES_5:ADC_SAMPLETIME_64CYCLES_5;
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
#if SCAN_ADC_PHASE_PAIR
    adc_configured=true;
#endif
    return arm_scan();
}
#if SCAN_ADC_PHASE_PAIR
static bool valid_tuning(uint32_t hz,uint8_t filter)
{
    return (hz==8000U||hz==16000U||hz==32000U||hz==64000U||hz==96000U)&&
           (filter==1U||filter==2U||filter==4U||filter==6U||filter==8U);
}
bool adc_scan_parse_tuning(const uint8_t payload[5],uint32_t *hz,uint8_t *filter)
{
    *hz=(uint32_t)payload[0]|((uint32_t)payload[1]<<8)|((uint32_t)payload[2]<<16)|((uint32_t)payload[3]<<24);
    *filter=payload[4];
    return valid_tuning(*hz,*filter);
}
/* Rate changes do not change the ADC clock, input ranks or acquisition
 * window. HAL_ADC_Stop_DMA leaves the ADC calibration configuration intact. */
static bool rearm_rate(uint32_t hz,uint8_t filter)
{
    adc_scan_stop();
    active_rate=hz;active_filter=filter;
    const uint32_t cycles=C77_CPU_HZ/hz;
    ring_cycles=ASYNC_RING_SCANS*cycles;
    copy_margin_cycles=(ASYNC_RING_SCANS-ASYNC_AVERAGE_SCANS)*cycles;
    reset_scan_state();
    __HAL_TIM_SET_AUTORELOAD(&scan_timer,SCAN_TIMER_HZ/hz-1U);
    __HAL_TIM_SET_COUNTER(&scan_timer,0U);
    TIM3->EGR=TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(&scan_timer,TIM_FLAG_UPDATE);
    return arm_scan();
}
bool adc_scan_tune(uint32_t hz,uint8_t filter)
{
    if(!adc_configured||!valid_tuning(hz,filter))return false;
    if(scan_state.running&&active_rate==hz){
        set_scan_filter(filter);
        return true;
    }
    const uint32_t old_rate=active_rate;
    const uint8_t old_filter=active_filter;
    if(rearm_rate(hz,filter))return true;
    (void)rearm_rate(old_rate,old_filter);
    return false;
}
#endif
