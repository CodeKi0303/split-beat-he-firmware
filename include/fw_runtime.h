#ifndef C86_FW_RUNTIME_H
#define C86_FW_RUNTIME_H
#include <stdbool.h>
#include <stdint.h>
/* Main-context only. No automatic restart after an interrupted transfer. */
void fw_runtime_init(uint8_t mode);
void fw_runtime_poll(void);
bool fw_runtime_request(const uint8_t packet[64]);
bool fw_runtime_busy(void);
bool fw_runtime_maintenance(void);
bool fw_runtime_relay_power(void);
bool fw_boot_cookie_take(void);
bool fw_boot_output_hold_take(void);
void fw_boot_reset(bool enter);
#endif
