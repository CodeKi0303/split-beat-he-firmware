#ifndef C77_INPUT_SCAN_H
#define C77_INPUT_SCAN_H
#include <stdint.h>
typedef struct { uint8_t raw,stable; uint32_t changed_us; } c77_debounce_t;
typedef struct { uint8_t previous,initialized; int8_t partial; uint16_t total; uint32_t invalid; } c77_quadrature_t;
uint8_t c77_debounce(c77_debounce_t *s,uint8_t raw,uint32_t now_us);
uint16_t c77_quadrature(c77_quadrature_t *s,uint8_t ab);
#endif
