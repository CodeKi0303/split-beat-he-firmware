#include "hardware_profile.h"
#if HARDWARE_REVISION==86
#include "fw_runtime.h"
#include "platform.h"
#include "stm32h7xx_hal.h"
#define COOKIE 0x33445055UL
#define OUTPUT_HOLD_COOKIE 0x46464F48UL
static void backup_access(void)
{
    __HAL_RCC_RTC_CLK_ENABLE();
    PWR->CR1|=PWR_CR1_DBP;
    for(unsigned n=0;n<100000U;n++)if(PWR->CR1&PWR_CR1_DBP)return;
    /* No unbounded wait or flash write if backup access is unavailable. */
    Error_Handler();
}
bool fw_boot_cookie_take(void)
{
    backup_access();
    bool force=RTC->BKP0R==COOKIE&&RTC->BKP1R==(uint32_t)~COOKIE;
    RTC->BKP0R=0;RTC->BKP1R=0;__DSB();
    PWR->CR1&=~PWR_CR1_DBP;return force;
}
void fw_boot_reset(bool enter)
{
    backup_access();RTC->BKP0R=enter?COOKIE:0;RTC->BKP1R=enter?(uint32_t)~COOKIE:0;
#if ROLE_LEFT && !FW_UPDATE_BOOT
    if(enter){RTC->BKP2R=OUTPUT_HOLD_COOKIE;RTC->BKP3R=(uint32_t)~OUTPUT_HOLD_COOKIE;}
#endif
    __DSB();PWR->CR1&=~PWR_CR1_DBP;board_reboot();
}
#if ROLE_LEFT && !FW_UPDATE_BOOT
bool fw_boot_output_hold_take(void)
{
    backup_access();
    bool hold=RTC->BKP2R==OUTPUT_HOLD_COOKIE&&RTC->BKP3R==(uint32_t)~OUTPUT_HOLD_COOKIE;
    RTC->BKP2R=0;RTC->BKP3R=0;__DSB();
    PWR->CR1&=~PWR_CR1_DBP;
    return hold;
}
#endif
#endif
