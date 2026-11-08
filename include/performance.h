#ifndef C86_PERFORMANCE_H
#define C86_PERFORMANCE_H
#include <stdbool.h>
#include <stdint.h>
bool performance_command(const uint8_t query[64],uint8_t reply[64]);
bool performance_busy_reply(const uint8_t query[64],uint8_t reply[64]);
/* Main-context only. Counters wrap modulo 2^32; first loop has no gap. */
void performance_loop_begin(uint32_t now_us);
void performance_sample_update(unsigned side,uint32_t sequence);
#endif
