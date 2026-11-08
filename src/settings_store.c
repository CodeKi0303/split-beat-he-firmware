#include "settings_store.h"
#include "flash_layout.h"
#include <stddef.h>
#include <string.h>

#define STORE_BASE H723_SETTINGS_BASE
#define SECTOR_BYTES H723_SECTOR_BYTES
#define RECORD_BYTES 576U
#define WORD_BYTES 32U
#define RECORDS_PER_SECTOR (SECTOR_BYTES / RECORD_BYTES)
#define RECORD_MAGIC 0x37374B43UL /* CK77 */
#define COMMIT_MAGIC 0x54494D43UL /* CMIT */
#define FORMAT_VERSION 77U
#define PAYLOAD_BYTES C77_CONFIG_BYTES
#define COMMIT_OFFSET 544U

static uint32_t last_error;

_Static_assert(H723_SETTINGS_SECTOR_COUNT == 2U, "Journal requires two sectors");
_Static_assert(STORE_BASE == H723_FLASH_ORIGIN + H723_SETTINGS_FIRST_SECTOR * SECTOR_BYTES,
               "Settings erase sectors must match the read/write addresses");
_Static_assert(STORE_BASE + 2U * SECTOR_BYTES == H723_FLASH_END,
               "Settings journal must end at physical flash capacity");

/* Retained in ELF/BIN so the verifier compares compiled C geometry with the
 * linker symbols and requested device profile, not just the image's length. */
#ifndef SETTINGS_STORE_HOST_TEST
__attribute__((section(".settings_layout"), used))
#endif
const uint32_t settings_flash_layout[] = {
    H723_FLASH_BYTES, STORE_BASE, H723_SETTINGS_FIRST_SECTOR,
    SECTOR_BYTES, H723_SETTINGS_SECTOR_COUNT
};

#ifdef SETTINGS_STORE_HOST_TEST
#include "settings_flash_mock.h"
#else
#include "stm32h7xx_hal.h"
_Static_assert(FLASH_SECTOR_SIZE == SECTOR_BYTES, "Unexpected H723 flash sector size");
_Static_assert(FLASH_NB_32BITWORD_IN_FLASHWORD * 4U == WORD_BYTES, "Expected 256-bit flashwords");

static int flash_layout_valid(void)
{
    /* Refuse settings access if a VG image was accidentally put on a VE part
     * (or vice versa). Never probe/erase an out-of-device settings address. */
    return *(volatile const uint16_t *)FLASHSIZE_BASE == H723_FLASH_BYTES / 1024U;
}

static int flash_ready(void)
{
    return __get_IPSR() == 0U && (SCB->CCR & SCB_CCR_DC_Msk) == 0U;
}

static int flash_read(uint32_t offset, uint8_t *out, uint32_t length)
{
    if (offset > 2U * SECTOR_BYTES || length > 2U * SECTOR_BYTES - offset ||
        (offset & 3U) != 0U || (length & 3U) != 0U) return 0;
    /* Interrupted flashword programming may leave uncorrectable ECC. ARM's
     * BFHFNMIGN with FAULTMASK permits a bounded data-bus probe at priority -1.
     * Treat ECC/BFSR errors as an occupied invalid record, never usable data.
     * No replacement HardFault/BusFault handlers or instruction skips needed.
     * Keep this scope limited to known flash addresses and SRAM stores. */
    uint32_t saved_faultmask = __get_FAULTMASK();
    uint32_t saved_ignore = SCB->CCR & SCB_CCR_BFHFNMIGN_Msk;
    __disable_fault_irq();
    SCB->CCR |= SCB_CCR_BFHFNMIGN_Msk;
    uint32_t prior_busfaults = SCB->CFSR & SCB_CFSR_BUSFAULTSR_Msk;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_DBECCERR_BANK1 | FLASH_FLAG_SNECCERR_BANK1);
    __DSB(); __ISB();
    for (uint32_t i = 0; i < length; i += 4U) {
        uint32_t word = *(volatile const uint32_t *)(uintptr_t)(STORE_BASE + offset + i);
        memcpy(out + i, &word, sizeof word);
    }
    __DSB();
    uint32_t ecc = FLASH->SR1 & FLASH_FLAG_DBECCERR_BANK1;
    uint32_t busfaults = (SCB->CFSR & SCB_CFSR_BUSFAULTSR_Msk) & ~prior_busfaults;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_DBECCERR_BANK1 | FLASH_FLAG_SNECCERR_BANK1);
    SCB->CFSR = busfaults; /* W1C only faults raised by this probe. */
    SCB->CCR = (SCB->CCR & ~SCB_CCR_BFHFNMIGN_Msk) | saved_ignore;
    __DSB(); __ISB();
    __set_FAULTMASK(saved_faultmask);
    return ecc == 0U && busfaults == 0U;
}

static int flash_unlock(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1 | FLASH_FLAG_EOP_BANK1);
    return HAL_FLASH_Unlock() == HAL_OK;
}
static void flash_lock(void) { (void)HAL_FLASH_Lock(); }
static int flash_erase(unsigned sector)
{
    if (sector > 1U) return 0;
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t failed_sector = UINT32_MAX;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_1;
    erase.Sector = H723_SETTINGS_FIRST_SECTOR + sector;
    erase.NbSectors = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3; /* Board supply 3.3 V. */
    return HAL_FLASHEx_Erase(&erase, &failed_sector) == HAL_OK;
}
static int flash_program(uint32_t offset, const uint8_t *word)
{
    if (offset > 2U * SECTOR_BYTES - WORD_BYTES || (offset & (WORD_BYTES - 1U)) != 0U) return 0;
    uint32_t aligned[8];
    memcpy(aligned, word, sizeof aligned);
    return HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, STORE_BASE + offset,
                             (uint32_t)(uintptr_t)aligned) == HAL_OK;
}
#endif

static uint16_t get16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4U; ++i) p[i] = (uint8_t)(v >> (8U * i)); }

static uint32_t crc_update(uint32_t crc, const uint8_t *data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
    }
    return crc;
}
static uint32_t record_crc(const uint8_t *record)
{
    uint32_t crc = crc_update(UINT32_MAX, record, 12U);
    return ~crc_update(crc, record + 16U, COMMIT_OFFSET - 16U);
}

static void make_commit(uint8_t *out, uint32_t generation, uint32_t crc)
{
    put32(out, COMMIT_MAGIC); put32(out + 4, ~COMMIT_MAGIC);
    put32(out + 8, generation); put32(out + 12, ~generation);
    put32(out + 16, crc); put32(out + 20, ~crc);
    put32(out + 24, RECORD_MAGIC); put32(out + 28, ~RECORD_MAGIC);
}
static void encode_record(uint8_t *record, const c77_config *profile, uint32_t generation, bool output_enabled)
{
    memset(record, 0, RECORD_BYTES);
    put32(record, RECORD_MAGIC); put16(record + 4, FORMAT_VERSION);
    put16(record + 6, PAYLOAD_BYTES); put32(record + 8, generation);
    record[16] = output_enabled;
    c77_config_encode(profile, record + 32);
    uint32_t crc = record_crc(record);
    put32(record + 12, crc);
    make_commit(record + COMMIT_OFFSET, generation, crc);
}
static int decode_record(const uint8_t *record, c77_config *profile, uint32_t *generation, bool *output_enabled)
{
    if (get32(record) != RECORD_MAGIC || get16(record + 4) != FORMAT_VERSION ||
        get16(record + 6) != PAYLOAD_BYTES || get32(record + 8) == 0U ||
        get32(record + 12) != record_crc(record)) return 0;
    if (record[16] > 1U) return 0;
    for (unsigned i = 17; i < 32U; ++i) if (record[i] != 0U) return 0;
    for(unsigned i=32U+PAYLOAD_BYTES;i<COMMIT_OFFSET;i++) if(record[i]) return 0;
    c77_config decoded;
    if(!c77_config_decode(&decoded,record+32)) return 0;
    uint8_t commit[WORD_BYTES];
    make_commit(commit, get32(record + 8), get32(record + 12));
    if (memcmp(record + COMMIT_OFFSET, commit, WORD_BYTES) != 0) return 0;
    if(profile != NULL) *profile=decoded;
    if (generation != NULL) *generation = get32(record + 8);
    if (output_enabled != NULL) *output_enabled = record[16] != 0U;
    return 1;
}
static int all_erased(const uint8_t *data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) if (data[i] != 0xFFU) return 0;
    return 1;
}

typedef struct {
    int found;
    unsigned sector;
    uint32_t generation, next_slot[2];
    c77_config profile;
    bool output_enabled;
} scan_result_t;

static void scan_journal(scan_result_t *result)
{
    memset(result, 0, sizeof *result);
    uint8_t record[RECORD_BYTES];
    for (unsigned sector = 0; sector < 2U; ++sector) {
        for (uint32_t slot = 0; slot < RECORDS_PER_SECTOR; ++slot) {
            uint32_t address = sector * SECTOR_BYTES + slot * RECORD_BYTES;
            if (!flash_read(address, record, sizeof record)) {
                result->next_slot[sector] = slot + 1U;
                continue; /* Unreadable/torn ECC is occupied, never reused. */
            }
            if (all_erased(record, sizeof record)) continue;
            result->next_slot[sector] = slot + 1U;
            uint32_t generation;
            if (decode_record(record, NULL, &generation, NULL) &&
                (!result->found || (int32_t)(generation - result->generation) > 0)) {
                result->found = 1;
                result->sector = sector;
                result->generation = generation;
                (void)decode_record(record, &result->profile, NULL, &result->output_enabled);
            }
        }
    }
}

uint32_t settings_store_last_error(void) { return last_error; }

int settings_store_load(c77_config *profile, uint32_t *generation, bool *output_enabled)
{
    if (output_enabled != NULL) *output_enabled = false;
    last_error = SETTINGS_ERROR_NONE;
    if (profile == NULL) { last_error = SETTINGS_ERROR_ARGUMENT; return 0; }
    if (!flash_layout_valid()) { last_error = SETTINGS_ERROR_LAYOUT; return 0; }
    if (!flash_ready()) { last_error = SETTINGS_ERROR_CACHE; return 0; }
    scan_result_t scan;
    scan_journal(&scan);
    if (!scan.found) return 0;
    *profile = scan.profile;
    if (generation != NULL) *generation = scan.generation;
    if (output_enabled != NULL) *output_enabled = scan.output_enabled;
    return 1;
}

int settings_store_save(const c77_config *profile, uint32_t *new_generation, bool output_enabled)
{
    last_error = SETTINGS_ERROR_NONE;
    if (!c77_config_valid(profile)) {
        last_error = SETTINGS_ERROR_ARGUMENT; return 0;
    }
    if (!flash_layout_valid()) { last_error = SETTINGS_ERROR_LAYOUT; return 0; }
    if (!flash_ready()) { last_error = SETTINGS_ERROR_CACHE; return 0; }
    scan_result_t scan;
    scan_journal(&scan);
    unsigned sector = scan.found ? scan.sector : 0U;
    uint32_t slot = scan.next_slot[sector];
    int erase_needed = slot >= RECORDS_PER_SECTOR;
    if (erase_needed) { sector ^= 1U; slot = 0U; }
    uint32_t generation = scan.found ? scan.generation + 1U : 1U;
    if (generation == 0U) generation = 1U;
    uint8_t record[RECORD_BYTES], verify[RECORD_BYTES];
    encode_record(record, profile, generation, output_enabled);
    if (!flash_unlock()) { last_error = SETTINGS_ERROR_UNLOCK; return 0; }
    if (erase_needed && !flash_erase(sector)) {
        last_error = SETTINGS_ERROR_ERASE;
        goto done;
    }
    uint32_t offset = sector * SECTOR_BYTES + slot * RECORD_BYTES;
    if (!flash_read(offset, verify, sizeof verify) || !all_erased(verify, sizeof verify)) {
        last_error = SETTINGS_ERROR_VERIFY;
        goto done;
    }
    /* Commit occupies a fresh flashword and is programmed only after the
     * header and payload read back exactly. No flashword is programmed twice. */
    for (uint32_t i = 0; i < COMMIT_OFFSET; i += WORD_BYTES) {
        if (!flash_program(offset + i, record + i)) {
            last_error = SETTINGS_ERROR_PROGRAM; goto done;
        }
    }
    if (!flash_read(offset, verify, COMMIT_OFFSET) || memcmp(verify, record, COMMIT_OFFSET) != 0) {
        last_error = SETTINGS_ERROR_VERIFY; goto done;
    }
    if (!flash_program(offset + COMMIT_OFFSET, record + COMMIT_OFFSET)) {
        last_error = SETTINGS_ERROR_PROGRAM; goto done;
    }
    if (!flash_read(offset, verify, sizeof verify) ||
        memcmp(verify, record, sizeof record) != 0 || !decode_record(verify, NULL, NULL, NULL)) {
        last_error = SETTINGS_ERROR_VERIFY; goto done;
    }
    if (new_generation != NULL) *new_generation = generation;
done:
    flash_lock();
    return last_error == SETTINGS_ERROR_NONE;
}
