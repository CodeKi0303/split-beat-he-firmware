/* Minimal SWD recovery entry, NOT an update/DFU bootloader. No flash writes.
 * App vector is fixed at0x08020000. Invalid vectors stay here for SWD recovery.
 * Keep sector0 separate so a later tested updater can replace this entry. */
#include <stdint.h>
__attribute__((noreturn)) static void halt(void){for(;;)__asm volatile("wfi");}
__attribute__((naked,noreturn)) static void jump(uint32_t sp __attribute__((unused)),uint32_t pc __attribute__((unused))){__asm volatile("msr msp,r0\n bx r1");}
__attribute__((noreturn)) void boot_reset(void){
    volatile const uint32_t *v=(volatile const uint32_t *)0x08020000;
    uint32_t sp=v[0],pc=v[1];
    if(sp<=0x20000000||sp>0x20020000||(sp&7)||!(pc&1)||pc<0x08020001||pc>=0x080c0000)halt();
    *(volatile uint32_t *)0xe000ed08=0x08020000;
    __asm volatile("dsb\n isb");jump(sp,pc);
}
__attribute__((section(".vectors"),used)) const uintptr_t vectors[16]={0x20020000,(uintptr_t)boot_reset,(uintptr_t)halt,(uintptr_t)halt};
