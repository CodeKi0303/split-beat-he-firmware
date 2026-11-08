#ifndef C77_SPLIT_CODEC_H
#define C77_SPLIT_CODEC_H
#include "platform.h"
#define C77_SPLIT_REQUEST_BYTES 8U
#define C77_SPLIT_REPLY_BYTES 32U
typedef struct {uint8_t data[32],used,reply;uint32_t crc_errors,framing_errors;} c77_split_parser_t;
void c77_split_request(uint8_t out[8],uint16_t sequence);
void c77_split_reply(uint8_t out[32],uint16_t sequence,const c77_sample_t *sample);
bool c77_split_decode(const uint8_t in[32],c77_sample_t *sample);
bool c77_split_feed(c77_split_parser_t *p,uint8_t byte,uint8_t out[32]);
uint16_t c77_split_sequence(const uint8_t *p);
#endif
