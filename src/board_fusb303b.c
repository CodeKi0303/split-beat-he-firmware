#include "board_internal.h"
#if defined(C86_CPU_PROFILE)
#include "cpu_profile.h"
#endif
#if HARDWARE_CC_FUSB303B && ROLE_LEFT
#include "cc_fusb303b.h"
#include "usb_device.h"
#include <string.h>
static I2C_HandleTypeDef bus;
static cc_fusb303b controller;
static volatile cc_io_result transfer_result;
static volatile bool transport_fault;
static bool clock_ready,bus_ready;
/* Visible to SWD; no new wire-protocol promise is made by these diagnostics. */
volatile uint32_t diagnostic_cc_state,diagnostic_cc_error;

static void cancel(void *context)
{
    (void)context;
    GPIOD->BSRR=GPIO_PIN_15; /* Fail off BEFORE resetting any peripheral. */
    GPIOE->BSRR=GPIO_PIN_15;
    HAL_NVIC_DisableIRQ(I2C1_EV_IRQn);HAL_NVIC_DisableIRQ(I2C1_ER_IRQn);
    __HAL_RCC_I2C1_FORCE_RESET();__HAL_RCC_I2C1_RELEASE_RESET();
    HAL_NVIC_ClearPendingIRQ(I2C1_EV_IRQn);HAL_NVIC_ClearPendingIRQ(I2C1_ER_IRQn);
    memset(&bus,0,sizeof bus);bus_ready=false;transfer_result=CC_IO_ERROR;transport_fault=false;
}
static bool initialize_bus(void)
{
    if(!clock_ready) return false;
    bus.Instance=I2C1;
    /* D2PCLK1=60MHz; PRESC=5 (100ns), SCLDEL=15 (1.6us), SDADEL=2
     * (200ns), SCLH=49 (5us), SCLL=59 (6us). <100kHz with filters/delays.
     * Conservative Standard-mode timing; verify rise/fall with R701/R702 4.7k. */
    bus.Init.Timing=0x50f2313bU;
    bus.Init.AddressingMode=I2C_ADDRESSINGMODE_7BIT;
    bus.Init.DualAddressMode=I2C_DUALADDRESS_DISABLE;
    bus.Init.OwnAddress2Masks=I2C_OA2_NOMASK;
    bus.Init.GeneralCallMode=I2C_GENERALCALL_DISABLE;
    bus.Init.NoStretchMode=I2C_NOSTRETCH_DISABLE;
    if(HAL_I2C_Init(&bus)!=HAL_OK || HAL_I2CEx_ConfigAnalogFilter(&bus,I2C_ANALOGFILTER_ENABLE)!=HAL_OK ||
       HAL_I2CEx_ConfigDigitalFilter(&bus,0)!=HAL_OK) return false;
    HAL_NVIC_SetPriority(I2C1_EV_IRQn,6,0);HAL_NVIC_SetPriority(I2C1_ER_IRQn,6,0);
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
    bus_ready=true;return true;
}
static bool start(void *context,bool write,uint8_t reg,uint8_t *value)
{
    (void)context;
    if(!bus_ready&&!initialize_bus()) return false;
    /* Avoid HAL's BUSY wait path; no blocking I2C transfers in the input loop. */
    if(__HAL_I2C_GET_FLAG(&bus,I2C_FLAG_BUSY)) return false;
    transfer_result=CC_IO_BUSY;
    HAL_StatusTypeDef result=write?
        HAL_I2C_Mem_Write_IT(&bus,FUSB303B_ADDRESS<<1,reg,I2C_MEMADD_SIZE_8BIT,value,1):
        HAL_I2C_Mem_Read_IT(&bus,FUSB303B_ADDRESS<<1,reg,I2C_MEMADD_SIZE_8BIT,value,1);
    return result==HAL_OK;
}
static cc_io_result poll(void *context) {(void)context;return transfer_result;}
static const cc_io io={NULL,start,poll,cancel};
void board_cc_init(void)
{
    RCC_PeriphCLKInitTypeDef clk={0};
    clk.PeriphClockSelection=RCC_PERIPHCLK_I2C1;
    clk.I2c123ClockSelection=RCC_I2C123CLKSOURCE_D2PCLK1;
    clock_ready=HAL_RCCEx_PeriphCLKConfig(&clk)==HAL_OK&&HAL_RCC_GetPCLK1Freq()==60000000U&&
        __HAL_RCC_GET_I2C1_SOURCE()==RCC_I2C1CLKSOURCE_D2PCLK1;
    __HAL_RCC_I2C1_CLK_ENABLE();
    GPIO_InitTypeDef pins={0};pins.Pin=GPIO_PIN_8|GPIO_PIN_9;
    pins.Mode=GPIO_MODE_AF_OD;pins.Pull=GPIO_NOPULL;pins.Speed=GPIO_SPEED_FREQ_LOW;
    pins.Alternate=GPIO_AF4_I2C1;HAL_GPIO_Init(GPIOB,&pins);
    cc_fusb303b_reset(&controller,&io);
}
void board_cc_stop(void) {cc_fusb303b_reset(&controller,&io);split_link_allow(false);}
void board_cc_service(void)
{
    uint32_t pins=GPIOE->IDR;
    cc_fusb303b_service(&controller,&io,board_micros(),usb_configured(),
        !(pins&GPIO_PIN_9),!(pins&GPIO_PIN_8),!(pins&GPIO_PIN_7));
    /* An error/USB IRQ can run while policy processing is in progress. Recheck
     * the live inhibit conditions atomically before committing GPIO outputs. */
    uint32_t irq=__get_PRIMASK();__disable_irq();
    pins=GPIOE->IDR;
    bool enable=controller.enable&&!transport_fault&&usb_configured()&&(pins&GPIO_PIN_9);
    bool allowed=enable&&controller.allow_power&&!(pins&GPIO_PIN_8);
    GPIOE->BSRR=enable?((uint32_t)GPIO_PIN_15<<16):GPIO_PIN_15;
    GPIOD->BSRR=allowed?((uint32_t)GPIO_PIN_15<<16):GPIO_PIN_15;
    __set_PRIMASK(irq);
    split_link_allow(allowed);
    diagnostic_cc_state=(uint32_t)controller.state;
    diagnostic_cc_error=(uint32_t)controller.error;
}
void I2C1_EV_IRQHandler(void)
{
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_BEGIN(cpu_irq);
#endif
    HAL_I2C_EV_IRQHandler(&bus);
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_IRQ_END(cpu_irq,CPU_PROF_IRQ_SYSTEM);
#endif
}
void I2C1_ER_IRQHandler(void)
{
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_BEGIN(cpu_irq);
#endif
    HAL_I2C_ER_IRQHandler(&bus);
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_IRQ_END(cpu_irq,CPU_PROF_IRQ_SYSTEM);
#endif
}
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *handle)
{if(handle==&bus) transfer_result=CC_IO_OK;}
void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *handle)
{if(handle==&bus) transfer_result=CC_IO_OK;}
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *handle)
{
    if(handle==&bus) {
        GPIOD->BSRR=GPIO_PIN_15;GPIOE->BSRR=GPIO_PIN_15;
        transport_fault=true;transfer_result=CC_IO_ERROR;
    }
}
#endif
