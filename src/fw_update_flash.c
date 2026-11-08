/* Fixed C86 application partition only. No option-byte writes, no boot erase,
 * no settings writes, and no address/sector selected by a host packet. */
#include "fw_update.h"
#include "fw_update_storage.h"
#include <string.h>

#define APP_SLOT_BYTES (FW_UPDATE_SLOT_END-FW_UPDATE_APP_BASE)
#define SECTOR_BYTES 0x20000UL
_Static_assert(FW_UPDATE_APP_BASE==0x08000000UL+SECTOR_BYTES, "App starts in sector1");
_Static_assert(FW_UPDATE_SLOT_END==0x08000000UL+3U*SECTOR_BYTES, "App ends after sector2");
_Static_assert(FW_UPDATE_METADATA_BASE+32U==FW_UPDATE_SLOT_END, "Metadata is the final flashword");

#ifdef FW_UPDATE_HOST_TEST
#include "fw_update_flash_mock.h"
uint16_t fw_update_flash_kib(void) { return fw_update_mock_kib(); }
void fw_update_flash_uid(uint8_t uid[12]) { fw_update_mock_uid(uid); }
static bool read_checked(uint32_t address, uint8_t *out, uint32_t length)
{ return fw_update_mock_read(address,out,length); }
#else
#include "stm32h7xx_hal.h"
_Static_assert(FLASH_SECTOR_SIZE==SECTOR_BYTES, "Unexpected H723 sector size");
_Static_assert(FLASH_NB_32BITWORD_IN_FLASHWORD*4U==32U, "Expected H723 256-bit programming unit");
uint16_t fw_update_flash_kib(void)
{ return *(volatile const uint16_t *)FLASHSIZE_BASE; }
void fw_update_flash_uid(uint8_t uid[12])
{
    uint32_t words[3]={HAL_GetUIDw0(),HAL_GetUIDw1(),HAL_GetUIDw2()};
    memcpy(uid,words,12);
}
static bool read_checked(uint32_t address, uint8_t *out, uint32_t length)
{
    /* Power loss during a flashword write can leave uncorrectable ECC. Ignore
     * only the data BusFault in this bounded FAULTMASK window and reject that
     * read. Never execute from the uncommitted app or blindly memcpy its data.
     * Same bounded ECC-probe model used by the existing settings journal. */
    uint32_t aligned=address&~3UL,end=(address+length+3U)&~3UL;
    uint32_t prior_mask=__get_FAULTMASK();
    uint32_t prior_ignore=SCB->CCR&SCB_CCR_BFHFNMIGN_Msk;
    uint32_t prior_busfaults=SCB->CFSR&SCB_CFSR_BUSFAULTSR_Msk;
    __disable_fault_irq(); SCB->CCR|=SCB_CCR_BFHFNMIGN_Msk;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_DBECCERR_BANK1|FLASH_FLAG_SNECCERR_BANK1);
    __DSB(); __ISB();
    for (uint32_t pos=aligned;pos<end;pos+=4U) {
        uint32_t value=*(volatile const uint32_t *)(uintptr_t)pos;
        for (unsigned i=0;i<4U;i++) {
            uint32_t at=pos+i;
            if (at>=address && at-address<length) out[at-address]=(uint8_t)(value>>(8U*i));
        }
    }
    __DSB();
    uint32_t ecc=FLASH->SR1&FLASH_FLAG_DBECCERR_BANK1;
    uint32_t faults=(SCB->CFSR&SCB_CFSR_BUSFAULTSR_Msk)&~prior_busfaults;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_DBECCERR_BANK1|FLASH_FLAG_SNECCERR_BANK1);
    SCB->CFSR=faults;
    SCB->CCR=(SCB->CCR&~SCB_CCR_BFHFNMIGN_Msk)|prior_ignore;
    __DSB(); __ISB(); __set_FAULTMASK(prior_mask);
    return !ecc && !faults;
}
#endif

bool fw_update_flash_read(uint32_t app_offset, uint8_t *out, uint32_t length)
{
    if (!out || !length || length>256U || app_offset>=APP_SLOT_BYTES ||
        length>APP_SLOT_BYTES-app_offset || fw_update_flash_kib()!=1024U) return false;
    return read_checked(FW_UPDATE_APP_BASE+app_offset,out,length);
}
bool fw_update_flash_boot_identity(uint8_t out[32])
{
    if (!out || fw_update_flash_kib()!=1024U) return false;
    return read_checked(FW_UPDATE_BOOT_BASE+FW_UPDATE_IDENTITY_OFFSET,out,32);
}

#if FW_UPDATE_BOOT && HARDWARE_REVISION==86
static bool write_ready(void)
{
    if (fw_update_flash_kib()!=1024U) return false;
#ifdef FW_UPDATE_HOST_TEST
    return fw_update_mock_ready();
#else
    return __get_IPSR()==0U && (SCB->CCR&SCB_CCR_DC_Msk)==0U &&
        SCB->VTOR==FW_UPDATE_BOOT_BASE;
#endif
}
static bool erase_one(unsigned sector)
{
    if (sector!=1U && sector!=2U) return false;
#ifdef FW_UPDATE_HOST_TEST
    return fw_update_mock_erase(sector);
#else
    FLASH_EraseInitTypeDef erase={0}; uint32_t failed=UINT32_MAX;
    erase.TypeErase=FLASH_TYPEERASE_SECTORS; erase.Banks=FLASH_BANK_1;
    erase.Sector=sector; erase.NbSectors=1U; erase.VoltageRange=FLASH_VOLTAGE_RANGE_3;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1|FLASH_FLAG_EOP_BANK1);
    if (HAL_FLASH_Unlock()!=HAL_OK) return false;
    HAL_StatusTypeDef result=HAL_FLASHEx_Erase(&erase,&failed);
    HAL_StatusTypeDef locked=HAL_FLASH_Lock();
    SCB_InvalidateICache(); __DSB(); __ISB();
    return result==HAL_OK && locked==HAL_OK && failed==UINT32_MAX;
#endif
}
bool fw_update_flash_erase(void)
{
    if (!write_ready()) return false;
    /* Erase the commit first. If power fails while replacing the vectors or
     * body in sector1, old metadata can no longer validate a half-written app. */
    return erase_one(2U) && erase_one(1U);
}
bool fw_update_flash_program(uint32_t app_offset, const uint8_t word[32])
{
    if (!word || !write_ready() || (app_offset&31U) ||
        app_offset>APP_SLOT_BYTES-32U) return false;
    uint8_t before[32],after[32];
    if (!fw_update_flash_read(app_offset,before,32)) return false;
    for (unsigned i=0;i<32U;i++) if (before[i]!=0xffU) return false;
#ifdef FW_UPDATE_HOST_TEST
    bool result=fw_update_mock_program(FW_UPDATE_APP_BASE+app_offset,word);
#else
    uint32_t aligned[8] __attribute__((aligned(32)));
    memcpy(aligned,word,32);
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1|FLASH_FLAG_EOP_BANK1);
    if (HAL_FLASH_Unlock()!=HAL_OK) return false;
    HAL_StatusTypeDef programmed=HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
        FW_UPDATE_APP_BASE+app_offset,(uint32_t)(uintptr_t)aligned);
    HAL_StatusTypeDef locked=HAL_FLASH_Lock();
    SCB_InvalidateICache(); __DSB(); __ISB();
    bool result=programmed==HAL_OK && locked==HAL_OK;
#endif
    return result && fw_update_flash_read(app_offset,after,32) && !memcmp(word,after,32);
}
#else
/* App images and legacy board builds cannot erase/program through this API,
 * even if a future caller accidentally passes MODE_BOOT to the dispatcher. */
bool fw_update_flash_erase(void) { return false; }
bool fw_update_flash_program(uint32_t app_offset, const uint8_t word[32])
{ (void)app_offset; (void)word; return false; }
#endif
