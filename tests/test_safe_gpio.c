/* Host HAL probe for the actual safe_gpio() implementation. No devices. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "hardware_profile.h"

typedef struct { uint32_t BSRR; } GPIO_TypeDef;
typedef struct { uint32_t Pin,Mode,Pull,Speed; } GPIO_InitTypeDef;
static GPIO_TypeDef port_a,port_b,port_c,port_d,port_e;
#define GPIOA (&port_a)
#define GPIOB (&port_b)
#define GPIOC (&port_c)
#define GPIOD (&port_d)
#define GPIOE (&port_e)
#define GPIO_PIN_0 (1U << 0)
#define GPIO_PIN_2 (1U << 2)
#define GPIO_PIN_3 (1U << 3)
#define GPIO_PIN_4 (1U << 4)
#define GPIO_PIN_5 (1U << 5)
#define GPIO_PIN_6 (1U << 6)
#define GPIO_PIN_7 (1U << 7)
#define GPIO_PIN_8 (1U << 8)
#define GPIO_PIN_9 (1U << 9)
#define GPIO_PIN_10 (1U << 10)
#define GPIO_PIN_11 (1U << 11)
#define GPIO_PIN_12 (1U << 12)
#define GPIO_PIN_13 (1U << 13)
#define GPIO_PIN_14 (1U << 14)
#define GPIO_PIN_15 (1U << 15)
#define GPIO_MODE_INPUT 0U
#define GPIO_MODE_OUTPUT_PP 1U
#define GPIO_MODE_OUTPUT_OD 2U
#define GPIO_NOPULL 0U
#define GPIO_SPEED_FREQ_LOW 0U
#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOD_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOE_CLK_ENABLE() ((void)0)
#define __HAL_RCC_SYSCFG_CLK_ENABLE() ((void)0)

static uint32_t modes[5][16],pulls[5][16],writes[5][16],configured[5];
static unsigned port_index(GPIO_TypeDef *port)
{
    if(port==GPIOA)return 0;
    if(port==GPIOB)return 1;
    if(port==GPIOC)return 2;
    if(port==GPIOD)return 3;
    assert(port==GPIOE);return 4;
}
static void HAL_GPIO_Init(GPIO_TypeDef *port,GPIO_InitTypeDef *pins)
{
    unsigned p=port_index(port);
    for(unsigned bit=0;bit<16;++bit)if(pins->Pin&(1U<<bit)) {
        /* Capture the preloaded output latch at the instant direction changes. */
        modes[p][bit]=pins->Mode;pulls[p][bit]=pins->Pull;
        writes[p][bit]=port->BSRR;configured[p]|=1U<<bit;
        if(pins->Mode!=GPIO_MODE_INPUT)assert(pins->Speed==GPIO_SPEED_FREQ_LOW);
    }
}

/* Generated from src/board.c, not a second copy of the GPIO implementation. */
#include "safe_gpio_under_test.inc"

static void pin(unsigned p,unsigned bit,uint32_t mode)
{
    assert(configured[p]&(1U<<bit));
    assert(modes[p][bit]==mode);assert(pulls[p][bit]==GPIO_NOPULL);
}
static void output(unsigned p,unsigned bit,uint32_t mode,int high)
{
    pin(p,bit,mode);
    uint32_t set=1U<<bit,reset=set<<16;
    assert((writes[p][bit]&(set|reset))==(high?set:reset));
}
int main(void)
{
    safe_gpio();
    /* Keep sensors disabled, RS485 quiet, and SWD pins untouched at boot. */
    output(4,2,GPIO_MODE_OUTPUT_PP,1);
    output(4,3,GPIO_MODE_OUTPUT_PP,1);
    output(3,4,GPIO_MODE_OUTPUT_PP,1);
    output(3,14,GPIO_MODE_OUTPUT_PP,0);
    const unsigned inputs[]={4,5,6,10,11,12,13};
    for(unsigned i=0;i<sizeof inputs/sizeof inputs[0];++i)pin(4,inputs[i],GPIO_MODE_INPUT);
    assert(configured[0]==0&&configured[1]==0&&configured[2]==0);
#if ROLE_LEFT
    output(4,15,HARDWARE_CC_FUSB303B?GPIO_MODE_OUTPUT_OD:GPIO_MODE_OUTPUT_PP,1);
    output(3,15,GPIO_MODE_OUTPUT_PP,1); /* Split power remains blocked. */
    output(3,10,GPIO_MODE_OUTPUT_PP,1); /* PHY stays in reset. */
    output(3,0,GPIO_MODE_OUTPUT_PP,0);  /* PHY oscillator remains off. */
    pin(4,7,GPIO_MODE_INPUT);pin(4,8,GPIO_MODE_INPUT);pin(4,9,GPIO_MODE_INPUT);
#else
    assert((configured[4]&(GPIO_PIN_7|GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_15))==0);
    assert((configured[3]&(GPIO_PIN_0|GPIO_PIN_10|GPIO_PIN_15))==0);
#endif
    printf("%s %s safe GPIO: mode, pull, preload and default power inhibition passed\n",
        HARDWARE_NAME,ROLE_LEFT?"left":"right");
    return 0;
}
