#ifndef C86_FW_UPDATE_STORAGE_H
#define C86_FW_UPDATE_STORAGE_H
#include <stdbool.h>
#include <stdint.h>

/* Internal fixed-partition backend, never accepts an erase address from wire. */
uint16_t fw_update_flash_kib(void);
void fw_update_flash_uid(uint8_t uid[12]);
bool fw_update_flash_read(uint32_t app_offset, uint8_t *out, uint32_t length);
bool fw_update_flash_boot_identity(uint8_t out[32]);
bool fw_update_flash_erase(void);
bool fw_update_flash_program(uint32_t app_offset, const uint8_t word[32]);

#endif
