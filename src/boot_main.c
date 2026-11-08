#include "stm32h7xx_hal.h"
#include "platform.h"
#include "fw_runtime.h"
#include "fw_update.h"
#include "update_link.h"
#if ROLE_LEFT
#include "usb_device.h"
#endif
volatile uint32_t diagnostic_fault_cfsr,diagnostic_fault_hfsr;
void _init(void){}
void _fini(void){}
void HardFault_Handler(void){diagnostic_fault_cfsr=SCB->CFSR;diagnostic_fault_hfsr=SCB->HFSR;Error_Handler();}
void MemManage_Handler(void){HardFault_Handler();}
void BusFault_Handler(void){HardFault_Handler();}
void UsageFault_Handler(void){HardFault_Handler();}
__attribute__((naked,noreturn)) static void jump(uint32_t sp __attribute__((unused)),uint32_t pc __attribute__((unused)))
{__asm volatile("movs r2,#0\n msr control,r2\n msr msp,r0\n dsb\n isb\n cpsie i\n bx r1");}
int main(void)
{
    /* Validate before HAL/clock/peripheral initialization. The app starts in
     * reset clock state with its own VTOR; no boot timers/IRQs are inherited. */
    SCB->VTOR=FW_UPDATE_BOOT_BASE;__DSB();__ISB();
    bool forced=fw_boot_cookie_take();uint32_t sp,pc;
    if(!forced&&fw_update_app_valid(&sp,&pc)){
        SCB->VTOR=FW_UPDATE_APP_BASE;__DSB();__ISB();jump(sp,pc);
    }
    board_init();if(board_flash_kib()!=1024U)Error_Handler();
    fw_update_init();board_update_start();
#if ROLE_LEFT
    usb_device_init();
#endif
    fw_runtime_init(FW_UPDATE_MODE_BOOT);
    for(;;){
#if ROLE_LEFT
        if(fw_runtime_relay_power())board_service();
        usb_device_poll();fw_runtime_poll();
        if(!fw_runtime_busy()){
            uint8_t packet[64];
            if(usb_vendor_read(packet)&&!fw_runtime_request(packet)){
                /* Boot is deliberately not the configuration application. */
            }
        }
#else
        fw_runtime_poll();
#endif
    }
}
