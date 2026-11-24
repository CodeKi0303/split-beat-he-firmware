#include "settings_store.h"
#include "flash_layout.h"
#include <stddef.h>
#include <string.h>

#define STORE_BASE H723_SETTINGS_BASE
#define SECTOR_BYTES H723_SECTOR_BYTES
/* Format 80 (firmware 0.4.0+) adds the four-profile bank and needs a larger
 * record: 2048 bytes, 64 per sector. Formats 77..79 use 576-byte records. A
 * sector holds one record size only: format 80 is first written at slot 0 of
 * a freshly erased sector, so the record at slot 0 tells the stride. */
#define RECORD_BYTES 2048U
#define OLD_RECORD_BYTES 576U
#define WORD_BYTES 32U
#define RECORDS_PER_SECTOR (SECTOR_BYTES / RECORD_BYTES)
#define OLD_RECORDS_PER_SECTOR (SECTOR_BYTES / OLD_RECORD_BYTES)
#define RECORD_MAGIC 0x37374B43UL /* CK77 */
#define COMMIT_MAGIC 0x54494D43UL /* CMIT */
/* 80 (0.4.0+): format 79 plus the profile bank. 79 (0.3.46+): format 78 plus
 * the 30-byte custom curve. 78 (0.3.43+): travel thresholds in 0..10000. 77:
 * 0..1000, converted. 77..79 load as a bank with one profile; 77/78 also with
 * the identity custom curve, so an update keeps the saved profile. Firmware
 * <=0.3.49 does not read format 80 and falls back to an older generation in
 * the other sector (or defaults); <=0.3.45 also rejects 79, <=0.3.42 also 78. */
#define FORMAT_VERSION 80U
#define CURVE_FORMAT_VERSION 79U
#define FINE_FORMAT_VERSION 78U
#define LEGACY_FORMAT_VERSION 77U
#define PAYLOAD_BYTES (C77_CONFIG_BYTES + C77_CURVE_BYTES + C77_PROFILE_BANK_BYTES)
#define CURVE_PAYLOAD_BYTES (C77_CONFIG_BYTES + C77_CURVE_BYTES)
#define OLD_PAYLOAD_BYTES C77_CONFIG_BYTES
#define COMMIT_OFFSET (RECORD_BYTES - WORD_BYTES)
#define OLD_COMMIT_OFFSET 544U
_Static_assert(32U + PAYLOAD_BYTES <= COMMIT_OFFSET, "Settings payload must end before the commit word");
_Static_assert(32U + CURVE_PAYLOAD_BYTES <= OLD_COMMIT_OFFSET, "Format 79 payload must end before its commit word");
_Static_assert(SECTOR_BYTES % RECORD_BYTES == 0U, "Format 80 records must tile the sector");

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
static uint32_t record_crc(const uint8_t *record, uint32_t commit_offset)
{
    uint32_t crc = crc_update(UINT32_MAX, record, 12U);
    return ~crc_update(crc, record + 16U, commit_offset - 16U);
}

static void make_commit(uint8_t *out, uint32_t generation, uint32_t crc)
{
    put32(out, COMMIT_MAGIC); put32(out + 4, ~COMMIT_MAGIC);
    put32(out + 8, generation); put32(out + 12, ~generation);
    put32(out + 16, crc); put32(out + 20, ~crc);
    put32(out + 24, RECORD_MAGIC); put32(out + 28, ~RECORD_MAGIC);
}
/* Writes a format 80 record. slots[active] is refreshed from profile, so the
 * stored bank always matches the live configuration; a NULL bank stores the
 * live configuration as the only profile. */
static void encode_record(uint8_t *record, const c77_config *profile, const c77_profile_bank *bank,
                          uint32_t generation, bool output_enabled)
{
    c77_profile_bank stored;
    if (bank != NULL) stored = *bank; else c77_profiles_init(&stored, profile);
    c77_profile_capture(&stored.slots[stored.active], profile);
    memset(record, 0, RECORD_BYTES);
    put32(record, RECORD_MAGIC); put16(record + 4, FORMAT_VERSION);
    put16(record + 6, PAYLOAD_BYTES); put32(record + 8, generation);
    record[16] = output_enabled;
    c77_config_encode(profile, record + 32);
    c77_curve_encode(profile->curve, record + 32 + C77_CONFIG_BYTES);
    c77_profiles_encode(&stored, record + 32 + CURVE_PAYLOAD_BYTES);
    uint32_t crc = record_crc(record, COMMIT_OFFSET);
    put32(record + 12, crc);
    make_commit(record + COMMIT_OFFSET, generation, crc);
}
/* size is the stride the record was read with (RECORD_BYTES or OLD_RECORD_BYTES). */
static int decode_record(const uint8_t *record, uint32_t size, c77_config *profile, c77_profile_bank *bank,
                         uint32_t *generation, bool *output_enabled)
{
    const uint16_t version = get16(record + 4);
    const int large = version == FORMAT_VERSION;
    const uint32_t payload = large ? PAYLOAD_BYTES : version == CURVE_FORMAT_VERSION ? CURVE_PAYLOAD_BYTES : OLD_PAYLOAD_BYTES;
    const uint32_t commit_offset = large ? COMMIT_OFFSET : OLD_COMMIT_OFFSET;
    if (size != (large ? RECORD_BYTES : OLD_RECORD_BYTES)) return 0;
    if (get32(record) != RECORD_MAGIC ||
        (version != FORMAT_VERSION && version != CURVE_FORMAT_VERSION &&
         version != FINE_FORMAT_VERSION && version != LEGACY_FORMAT_VERSION) ||
        get16(record + 6) != payload || get32(record + 8) == 0U ||
        get32(record + 12) != record_crc(record, commit_offset)) return 0;
    if (record[16] > 1U) return 0;
    for (unsigned i = 17; i < 32U; ++i) if (record[i] != 0U) return 0;
    for (uint32_t i = 32U + payload; i < commit_offset; i++) if (record[i]) return 0;
    c77_config decoded;
    if(!(version == LEGACY_FORMAT_VERSION ? c77_config_decode_legacy(&decoded,record+32) :
         c77_config_decode(&decoded,record+32))) return 0;
    if(version >= CURVE_FORMAT_VERSION &&
       (!c77_curve_decode(decoded.curve,record+32+C77_CONFIG_BYTES) || !c77_config_valid(&decoded))) return 0;
    c77_profile_bank profiles;
    if (large) {
        if (!c77_profiles_decode(&profiles, record + 32 + CURVE_PAYLOAD_BYTES, &decoded)) return 0;
        /* The active slot must hold exactly the live per-profile fields:
         * re-encoding with it refreshed from the live keys gives the same bytes. */
        static uint8_t check[C77_PROFILE_BANK_BYTES];
        c77_profile_bank refreshed = profiles;
        c77_profile_capture(&refreshed.slots[refreshed.active], &decoded);
        c77_profiles_encode(&refreshed, check);
        if (memcmp(check, record + 32 + CURVE_PAYLOAD_BYTES, sizeof check) != 0) return 0;
    } else {
        c77_profiles_init(&profiles, &decoded);
    }
    uint8_t commit[WORD_BYTES];
    make_commit(commit, get32(record + 8), get32(record + 12));
    if (memcmp(record + commit_offset, commit, WORD_BYTES) != 0) return 0;
    if (profile != NULL) *profile = decoded;
    if (bank != NULL) *bank = profiles;
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
    uint32_t generation, next_slot[2], stride[2];
    c77_config profile;
    c77_profile_bank bank;
    bool output_enabled;
} scan_result_t;

/* Static: 2 KB records and the scan state do not belong on the 8 KB stack. */
static uint8_t scan_buffer[RECORD_BYTES], write_buffer[RECORD_BYTES], verify_buffer[RECORD_BYTES];
static scan_result_t scan_state;

static uint32_t sector_stride(unsigned sector)
{
    uint8_t head[8];
    if (!flash_read(sector * SECTOR_BYTES, head, sizeof head)) return OLD_RECORD_BYTES;
    if (all_erased(head, sizeof head)) return RECORD_BYTES;
    return get32(head) == RECORD_MAGIC && get16(head + 4) == FORMAT_VERSION ? RECORD_BYTES : OLD_RECORD_BYTES;
}

static void scan_journal(scan_result_t *result)
{
    memset(result, 0, sizeof *result);
    uint8_t *record = scan_buffer;
    for (unsigned sector = 0; sector < 2U; ++sector) {
        const uint32_t stride = sector_stride(sector);
        result->stride[sector] = stride;
        for (uint32_t slot = 0; slot < SECTOR_BYTES / stride; ++slot) {
            uint32_t address = sector * SECTOR_BYTES + slot * stride;
            if (!flash_read(address, record, stride)) {
                result->next_slot[sector] = slot + 1U;
                continue; /* Unreadable/torn ECC is occupied, never reused. */
            }
            if (all_erased(record, stride)) continue;
            result->next_slot[sector] = slot + 1U;
            uint32_t generation;
            if (decode_record(record, stride, NULL, NULL, &generation, NULL) &&
                (!result->found || (int32_t)(generation - result->generation) > 0)) {
                result->found = 1;
                result->sector = sector;
                result->generation = generation;
                (void)decode_record(record, stride, &result->profile, &result->bank, NULL, &result->output_enabled);
            }
        }
    }
}

uint32_t settings_store_last_error(void) { return last_error; }

int settings_store_load(c77_config *profile, c77_profile_bank *bank, uint32_t *generation, bool *output_enabled)
{
    if (output_enabled != NULL) *output_enabled = false;
    last_error = SETTINGS_ERROR_NONE;
    if (profile == NULL) { last_error = SETTINGS_ERROR_ARGUMENT; return 0; }
    if (!flash_layout_valid()) { last_error = SETTINGS_ERROR_LAYOUT; return 0; }
    if (!flash_ready()) { last_error = SETTINGS_ERROR_CACHE; return 0; }
    scan_result_t *scan = &scan_state;
    scan_journal(scan);
    if (!scan->found) return 0;
    *profile = scan->profile;
    if (bank != NULL) *bank = scan->bank;
    if (generation != NULL) *generation = scan->generation;
    if (output_enabled != NULL) *output_enabled = scan->output_enabled;
    return 1;
}

int settings_store_save(const c77_config *profile, const c77_profile_bank *bank, uint32_t *new_generation, bool output_enabled)
{
    last_error = SETTINGS_ERROR_NONE;
    if (!c77_config_valid(profile) || (bank != NULL && !c77_profiles_valid(bank, profile))) {
        last_error = SETTINGS_ERROR_ARGUMENT; return 0;
    }
    if (!flash_layout_valid()) { last_error = SETTINGS_ERROR_LAYOUT; return 0; }
    if (!flash_ready()) { last_error = SETTINGS_ERROR_CACHE; return 0; }
    scan_result_t *scan = &scan_state;
    scan_journal(scan);
    unsigned sector = scan->found ? scan->sector : 0U;
    uint32_t slot = scan->next_slot[sector];
    /* Format 80 never shares a sector with 576-byte records. */
    int erase_needed = scan->stride[sector] != RECORD_BYTES || slot >= RECORDS_PER_SECTOR;
    if (erase_needed) { sector ^= 1U; slot = 0U; }
    uint32_t generation = scan->found ? scan->generation + 1U : 1U;
    if (generation == 0U) generation = 1U;
    uint8_t *record = write_buffer, *verify = verify_buffer;
    encode_record(record, profile, bank, generation, output_enabled);
    if (!flash_unlock()) { last_error = SETTINGS_ERROR_UNLOCK; return 0; }
    if (erase_needed && !flash_erase(sector)) {
        last_error = SETTINGS_ERROR_ERASE;
        goto done;
    }
    uint32_t offset = sector * SECTOR_BYTES + slot * RECORD_BYTES;
    if (!flash_read(offset, verify, RECORD_BYTES) || !all_erased(verify, RECORD_BYTES)) {
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
    if (!flash_read(offset, verify, RECORD_BYTES) ||
        memcmp(verify, record, RECORD_BYTES) != 0 || !decode_record(verify, RECORD_BYTES, NULL, NULL, NULL, NULL)) {
        last_error = SETTINGS_ERROR_VERIFY; goto done;
    }
    if (new_generation != NULL) *new_generation = generation;
done:
    flash_lock();
    return last_error == SETTINGS_ERROR_NONE;
}
