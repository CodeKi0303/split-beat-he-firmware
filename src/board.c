#include "board_internal.h"
#if defined(C86_CPU_PROFILE)
#include "cpu_profile.h"
#endif
extern volatile uint32_t diagnostic_boot_stage;
static void check(HAL_StatusTypeDef s) { if(s!=HAL_OK) Error_Handler(); }
static void board_clock_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};
    RCC_PeriphCLKInitTypeDef peripheral = {0};
    PLL1_ClocksTypeDef pll1_clocks = {0};
    PLL2_ClocksTypeDef pll2_clocks = {0};
    diagnostic_boot_stage = 2U; /* Supply / voltage scaling. */
    check(HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY));
    /* Use the STM32H723 LDO/VOS0 supply path. Keep CPU/bus
     * clocks low, but do not assume VOS3 permits a120MHz asynchronous kernel. */
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);
    uint32_t start = HAL_GetTick();
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {
        if (HAL_GetTick() - start > 100U) Error_Handler();
    }
    /* C77 external oscillator = 25 MHz bypass. VCO = 25/5*96 = 480 MHz;
     * PLL1P = 240 MHz CPU, PLL1Q = 48 MHz USB (crystal referenced). */
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_CSI;
    osc.HSEState = RCC_HSE_BYPASS;
    osc.CSIState = RCC_CSI_ON;
    osc.CSICalibrationValue = RCC_CSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = 5;
    osc.PLL.PLLN = 96;
    osc.PLL.PLLP = 2;
    osc.PLL.PLLQ = 10;
    osc.PLL.PLLR = 4;
    osc.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
    osc.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
    osc.PLL.PLLFRACN = 0;
    diagnostic_boot_stage = 3U; /* HSE / PLL startup. */
    check(HAL_RCC_OscConfig(&osc));
    /* DS13313 USART I/O characterization uses VOS0, fastest GPIO slew and
     * I/O compensation. CSI must run for the compensation cell. */
    HAL_EnableCompensationCell();
    start = HAL_GetTick();
    while ((SYSCFG->CCCSR & SYSCFG_CCCSR_READY) == 0U) {
        if (HAL_GetTick() - start > 100U) Error_Handler();
    }
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                    RCC_CLOCKTYPE_D1PCLK1 | RCC_CLOCKTYPE_D3PCLK1;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.SYSCLKDivider = RCC_SYSCLK_DIV1;
    clk.AHBCLKDivider = RCC_HCLK_DIV2; /* HCLK 120 MHz. */
    clk.APB1CLKDivider = RCC_APB1_DIV2;
    clk.APB2CLKDivider = RCC_APB2_DIV2;
    clk.APB3CLKDivider = RCC_APB3_DIV2;
    clk.APB4CLKDivider = RCC_APB4_DIV2;
    diagnostic_boot_stage = 4U; /* CPU / bus clocks. */
    check(HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2));
    diagnostic_boot_stage = 5U; /* Peripheral clocks / USB clock verification. */
    peripheral.PeriphClockSelection = RCC_PERIPHCLK_CKPER | RCC_PERIPHCLK_USB | RCC_PERIPHCLK_USART3;
    peripheral.CkperClockSelection = RCC_CLKPSOURCE_HSE;
    peripheral.PeriphClockSelection |= RCC_PERIPHCLK_ADC;
#if SCAN_ADC_USE_PLL2
    peripheral.AdcClockSelection = RCC_ADCCLKSOURCE_PLL2;
#else
    peripheral.AdcClockSelection = RCC_ADCCLKSOURCE_CLKP;
#endif
    /* Separate PLL2Q makes 120MHz USART kernel, independent of 60MHz APB1. */
    peripheral.PLL2.PLL2M=5; peripheral.PLL2.PLL2N=96;
    peripheral.PLL2.PLL2P=SCAN_ADC_PLL2_P_DIV; peripheral.PLL2.PLL2Q=4; peripheral.PLL2.PLL2R=4;
    peripheral.PLL2.PLL2RGE=RCC_PLL2VCIRANGE_2;
    peripheral.PLL2.PLL2VCOSEL=RCC_PLL2VCOWIDE;
    peripheral.PLL2.PLL2FRACN=0;
    peripheral.Usart234578ClockSelection=RCC_USART234578CLKSOURCE_PLL2;
    peripheral.UsbClockSelection = RCC_USBCLKSOURCE_PLL;
    check(HAL_RCCEx_PeriphCLKConfig(&peripheral));
    /* This pinned HAL does not implement USART in GetPeriphCLKFreq: it returns
     * zero for that selection. Verify PLL2Q and the actual USART mux directly. */
    HAL_RCCEx_GetPLL2ClockFreq(&pll2_clocks);
    if (pll2_clocks.PLL2_Q_Frequency != 120000000U ||
#if SCAN_ADC_USE_PLL2
        pll2_clocks.PLL2_P_Frequency != SCAN_ADC_KERNEL_HZ ||
        __HAL_RCC_GET_ADC_SOURCE() != RCC_ADCCLKSOURCE_PLL2 ||
#else
        __HAL_RCC_GET_ADC_SOURCE() != RCC_ADCCLKSOURCE_CLKP ||
#endif
        HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_ADC) != SCAN_ADC_KERNEL_HZ ||
        __HAL_RCC_GET_USART234578_SOURCE() != RCC_USART234578CLKSOURCE_PLL2 ||
        __HAL_RCC_GET_FLAG(RCC_FLAG_PLL2RDY) == RESET ||
        HAL_RCC_GetPCLK1Freq()!=60000000U || HAL_RCC_GetHCLKFreq()!=120000000U ||
        SystemCoreClock!=C77_CPU_HZ) Error_Handler();
    /* This pinned HAL's generic GetPeriphCLKFreq does not support USB and
     * returns zero for it. Read PLL1Q and the actual USB mux instead. */
    HAL_RCCEx_GetPLL1ClockFreq(&pll1_clocks);
    if (pll1_clocks.PLL1_Q_Frequency != 48000000U ||
        __HAL_RCC_GET_USB_SOURCE() != RCC_USBCLKSOURCE_PLL ||
        __HAL_RCC_GET_FLAG(RCC_FLAG_PLLRDY) == RESET) Error_Handler();
}

#include "input_scan.h"
#include "update_link.h"
#if ROLE_LEFT
#include "usb_device.h"
#endif
#include <string.h>
volatile uint32_t diagnostic_boot_stage,diagnostic_fatal_error;
static c77_quadrature_t encoders[2];
static bool scanning;
#if ROLE_LEFT && !HARDWARE_CC_FUSB303B
static bool cc_started,split_fault;
static uint32_t cc_started_at;
#endif

static void gpio_outputs(GPIO_TypeDef *port,uint16_t pins)
{
    GPIO_InitTypeDef g={0};
    g.Pin=pins;g.Mode=GPIO_MODE_OUTPUT_PP;g.Pull=GPIO_NOPULL;g.Speed=GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(port,&g);
}
static void safe_gpio(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();__HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();__HAL_RCC_GPIOD_CLK_ENABLE();__HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    /* Set output data before direction to avoid active-low power glitches.
     * SWD PA13/14 are deliberately never reconfigured. */
    GPIOE->BSRR=GPIO_PIN_2|GPIO_PIN_3;
    GPIOD->BSRR=GPIO_PIN_4|((uint32_t)GPIO_PIN_14<<16);
    gpio_outputs(GPIOE,GPIO_PIN_2|GPIO_PIN_3);
    gpio_outputs(GPIOD,GPIO_PIN_4|GPIO_PIN_14);
    GPIO_InitTypeDef g={0};
    g.Pin=GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_12|GPIO_PIN_13;
    g.Mode=GPIO_MODE_INPUT;g.Pull=GPIO_NOPULL;HAL_GPIO_Init(GPIOE,&g);
#if ROLE_LEFT
    GPIOE->BSRR=GPIO_PIN_15;
    GPIOD->BSRR=GPIO_PIN_15|GPIO_PIN_10|((uint32_t)GPIO_PIN_0<<16);
#if HARDWARE_CC_FUSB303B
    /* Match the C86 R609-DNP assembly using EN_N's internal pull-up.
     * FUSB303B/D Rev3 lists EN_N at 6.0 V absolute maximum; the old
     * distributor Rev2 PDF listed 2.0 V. Retain release/low control for
     * the assembled DNP configuration without assuming a 3.3 V violation.
     * Preload release above before enabling OD.
     * Use a separate descriptor so CC status inputs remain inputs. */
    GPIO_InitTypeDef enable_pin={0};
    enable_pin.Pin=GPIO_PIN_15;enable_pin.Mode=GPIO_MODE_OUTPUT_OD;
    enable_pin.Pull=GPIO_NOPULL;enable_pin.Speed=GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOE,&enable_pin);
#else
    gpio_outputs(GPIOE,GPIO_PIN_15);
#endif
    gpio_outputs(GPIOD,GPIO_PIN_15|GPIO_PIN_10|GPIO_PIN_0);
    g.Pin=GPIO_PIN_7|GPIO_PIN_8|GPIO_PIN_9;
    g.Mode=GPIO_MODE_INPUT;g.Pull=GPIO_NOPULL;HAL_GPIO_Init(GPIOE,&g);
#endif
}
void board_init(void)
{
    /* DMA ownership model explicitly requires D-cache off; instruction cache
     * may stay enabled. Bootloader must leave clocks/peripherals reset. */
    if(SCB->CCR&SCB_CCR_DC_Msk) SCB_DisableDCache();
    SCB_EnableICache();
    if(HAL_Init()!=HAL_OK) Error_Handler();
    safe_gpio();
    HAL_Delay(10); /* 25MHz oscillator worst-case startup, enabled by PD4. */
    board_clock_init();
#if ROLE_LEFT && HARDWARE_CC_FUSB303B
    board_cc_init();
#endif
    /* Initialize every DMA SRAM word before any byte store/read on NOLOAD
     * buffers. DMA cannot access the DTCM used by ordinary data/stack. */
    extern uint32_t __dma_start__,__dma_end__;
    for(uint32_t *p=&__dma_start__;p<&__dma_end__;p++) *p=0;
    __DMB();
    CoreDebug->DEMCR|=CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR=0xC5ACCE55U;DWT->CTRL|=DWT_CTRL_CYCCNTENA_Msk;
    __HAL_RCC_TIM2_CLK_ENABLE();
    TIM2->CR1=0;TIM2->PSC=119U;TIM2->ARR=0xffffffffU;TIM2->CNT=0;
    TIM2->EGR=TIM_EGR_UG;TIM2->SR=0;TIM2->CR1=TIM_CR1_CEN;
#if ROLE_LEFT
    GPIOD->BSRR=GPIO_PIN_0; /* PHY 24MHz OE, RESET remains asserted HIGH. */
    HAL_Delay(10);
    GPIOD->BSRR=(uint32_t)GPIO_PIN_10<<16; /* USB3300 RESET is active HIGH. */
    HAL_Delay(1);
#endif
    diagnostic_boot_stage=6;
}
uint32_t board_micros(void) {return TIM2->CNT;}
uint16_t board_flash_kib(void) {return *(volatile const uint16_t *)FLASHSIZE_BASE;}
void board_input_scan(c77_sample_t *sample)
{
    uint32_t p=GPIOE->IDR;
    /* Latest physical level only. Master owns key debounce, mapping and RT;
     * adding a second debounce here would unnecessarily increase latency. */
    sample->digital=(uint8_t)((!(p&GPIO_PIN_10))|((!(p&GPIO_PIN_13))<<1)|((!(p&GPIO_PIN_6))<<2));
    sample->encoders[0]=c77_quadrature(&encoders[0],(uint8_t)((p>>11)&3U));
    sample->encoders[1]=c77_quadrature(&encoders[1],(uint8_t)((p>>4)&3U));
    sample->errors+=encoders[0].invalid+encoders[1].invalid;
}
void board_start(void)
{
    memset(encoders,0,sizeof encoders);
    GPIOE->BSRR=(uint32_t)GPIO_PIN_2<<16; /* Hall rail on; VDDA remains always on. */
    HAL_Delay(10); /* Starting guard, actual sensor/rail settling measured later. */
    scanning=adc_scan_start();
    split_link_start();
#if !ROLE_LEFT
    split_link_allow(true);
#endif
}
void board_stop(void)
{
#if ROLE_LEFT
    GPIOD->BSRR=GPIO_PIN_15;GPIOE->BSRR=GPIO_PIN_15;
#if HARDWARE_CC_FUSB303B
    board_cc_stop();
#else
    cc_started=false;
#endif
#endif
    split_link_stop();adc_scan_stop();scanning=false;
    GPIOE->BSRR=GPIO_PIN_2|GPIO_PIN_3;
    GPIOD->BSRR=(uint32_t)GPIO_PIN_14<<16;
}
void board_update_start(void)
{
    /* Permanent boot recovery: communication only, no Hall rail or ADC. */
    split_link_start();split_update_mode(true);
#if !ROLE_LEFT
    split_link_allow(true);
#endif
}
void board_update_pause(void)
{
    adc_scan_stop();scanning=false;split_update_mode(true);
}
void board_update_resume(void)
{
    scanning=adc_scan_start();split_update_mode(false);
}
void board_update_power_off(void)
{
#if ROLE_LEFT && HARDWARE_CC_FUSB303B
    board_cc_stop();
#endif
}
bool board_local_snapshot(c77_sample_t *out)
{
    bool valid=adc_scan_snapshot(out);
    return valid&&scanning;
}
bool board_remote_snapshot(c77_sample_t *out) {return split_link_snapshot(out);}
void board_service(void)
{
#if ROLE_LEFT
#if HARDWARE_CC_FUSB303B
    board_cc_service();
#else
    /* Source strap/ID_N is the hardware attach condition. Never energize the
     * split connector just because a cable exists; USB configuration grants
     * the declared 500mA budget. Board power consumption still needs measuring.
     * TUSB320 I2C pins are reserved; no replacement-controller assumptions. */
    if(!usb_configured()) {
        GPIOD->BSRR=GPIO_PIN_15;GPIOE->BSRR=GPIO_PIN_15;
        split_link_allow(false);cc_started=false;split_fault=false;
        return;
    }
    if(!(GPIOE->IDR&GPIO_PIN_9)) split_fault=true;
    if(split_fault) {
        GPIOD->BSRR=GPIO_PIN_15;GPIOE->BSRR=GPIO_PIN_15;
        split_link_allow(false);return;
    }
    if(!cc_started) {
        GPIOD->BSRR=GPIO_PIN_15;
        GPIOE->BSRR=(uint32_t)GPIO_PIN_15<<16;
        cc_started=true;cc_started_at=board_micros();
        return;
    }
    if(board_micros()-cc_started_at<20000U) return;
    bool attached=(GPIOE->IDR&GPIO_PIN_8)==0U;
    if(attached) GPIOD->BSRR=(uint32_t)GPIO_PIN_15<<16;
    else GPIOD->BSRR=GPIO_PIN_15;
    split_link_allow(attached);
#endif
#endif
}
void board_reboot(void) {__DSB();NVIC_SystemReset();}
void SysTick_Handler(void)
{
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_BEGIN(cpu_irq);
#endif
    HAL_IncTick();
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_IRQ_END(cpu_irq,CPU_PROF_IRQ_SYSTEM);
#endif
}
void Error_Handler(void)
{
    diagnostic_fatal_error=diagnostic_boot_stage;
    __disable_irq();
#if ROLE_LEFT
    /* A fatal fault cannot reliably enqueue a neutral HID report. Ask the
     * hardware to detach so the host can discard its held-key state instead.
     * No HAL call, lock, FIFO wait or tick timeout is safe in this path. */
    if((RCC->AHB1ENR&RCC_AHB1ENR_USB1OTGHSEN)!=0U) {
        USB_OTG_DeviceTypeDef *device=(USB_OTG_DeviceTypeDef *)
            ((uintptr_t)USB_OTG_HS+USB_OTG_DEVICE_BASE);
        device->DCTL|=USB_OTG_DCTL_SDIS;
        __DSB();
    }
#endif
    /* Never leave the shared link driven or downstream supply commanded on. */
    __HAL_RCC_GPIOD_CLK_ENABLE();__HAL_RCC_GPIOE_CLK_ENABLE();
    GPIOD->BSRR=GPIO_PIN_15|((uint32_t)GPIO_PIN_14<<16);
    GPIOE->BSRR=GPIO_PIN_15|GPIO_PIN_3;
    __DSB();
    for(;;) __WFI();
}
