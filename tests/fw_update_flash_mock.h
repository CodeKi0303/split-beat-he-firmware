#ifndef C86_FW_UPDATE_FLASH_MOCK_H
#define C86_FW_UPDATE_FLASH_MOCK_H
#include <stdbool.h>
#include <stdint.h>
uint16_t fw_update_mock_kib(void);
void fw_update_mock_uid(uint8_t uid[12]);
bool fw_update_mock_ready(void);
bool fw_update_mock_read(uint32_t address, uint8_t *out, uint32_t length);
bool fw_update_mock_erase(unsigned sector);
bool fw_update_mock_program(uint32_t address, const uint8_t word[32]);
#endif
