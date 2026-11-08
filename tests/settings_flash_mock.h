#ifndef SETTINGS_FLASH_MOCK_H
#define SETTINGS_FLASH_MOCK_H
static int flash_layout_valid(void);
static int flash_ready(void);
static int flash_read(uint32_t offset, uint8_t *out, uint32_t length);
static int flash_unlock(void);
static void flash_lock(void);
static int flash_erase(unsigned sector);
static int flash_program(uint32_t offset, const uint8_t *word);
#endif
