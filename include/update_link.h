#ifndef C86_UPDATE_LINK_H
#define C86_UPDATE_LINK_H
#include <stdint.h>
#include <stdbool.h>
typedef struct { uint8_t bytes[64]; unsigned used; uint32_t errors; } update_frame_parser;
bool update_frame_feed(update_frame_parser *p,uint8_t byte,uint8_t out[64]);
bool update_frame_valid(const uint8_t p[64]);
uint16_t update_frame_crc(const uint8_t *p,unsigned n);
void update_frame_finish(uint8_t p[64]);
void split_update_mode(bool enable);
bool split_update_send(const uint8_t packet[64]);
bool split_update_read(uint8_t packet[64]);
bool split_update_tx_idle(void);
bool split_update_tx_succeeded(void);
#endif
