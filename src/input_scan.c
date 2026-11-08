#include "input_scan.h"
uint8_t c77_debounce(c77_debounce_t *s,uint8_t raw,uint32_t now_us)
{
    raw=!!raw;
    if(raw!=s->raw) {s->raw=raw;s->changed_us=now_us;}
    if(raw!=s->stable && now_us-s->changed_us>=5000U) s->stable=raw;
    return s->stable;
}
uint16_t c77_quadrature(c77_quadrature_t *s,uint8_t ab)
{
    /* Reject impossible two-bit jumps. Opposite bounce transitions cancel;
     * publish only complete four-transition cycles, not every contact edge. */
    static const int8_t delta[16]={0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
    ab&=3U;
    if(!s->initialized) {s->previous=ab;s->initialized=1;return s->total;}
    if((s->previous^ab)==3U) {s->invalid++;s->partial=0;}
    else s->partial+=delta[s->previous*4U+ab];
    s->previous=ab;
    if(s->partial>=4) {s->total++;s->partial=0;}
    if(s->partial<=-4) {s->total--;s->partial=0;}
    return s->total;
}
