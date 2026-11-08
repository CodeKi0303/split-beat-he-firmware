/* Deterministic stdin/stdout simulator executing the ACTUAL C protocol/core.
 * Each line is a64-byte hex request. Samples advance256times before each line.
 * This process never opens USB or generates operating-system key events. */
#include "keyboard.h"
#include <stdio.h>
#include <string.h>
static int nibble(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
int main(void){
    c77_config cfg;c77_defaults(&cfg);c77_engine e;c77_engine_init(&e,&cfg,0,false);uint32_t now=0,sequence=0;char line[256];
    while(fgets(line,sizeof line,stdin)){
        if(strlen(line)<128)return 2;
        uint8_t q[64],r[64];
        for(unsigned i=0;i<64;i++){int a=nibble(line[i*2]),b=nibble(line[i*2+1]);if(a<0||b<0)return 2;q[i]=(uint8_t)((a<<4)|b);}
        for(unsigned n=0;n<256;n++){
            now+=63;sequence++;
            for(unsigned side=0;side<2;side++){c77_sample_t s={0};s.valid=1;s.sequence=sequence;s.timestamp_us=now;for(unsigned i=0;i<7;i++)s.raw[i]=(uint16_t)((e.cal_state==3&&e.cal_key==side*7+i)?1000:3000);c77_update(&e,side,&s,now);}c77_tick(&e,now);
        }
        if(!c77_command(&e,q,r,now,2,1024))return 3;
        for(unsigned i=0;i<64;i++)printf("%02x",r[i]);
        puts("");fflush(stdout);
        if(e.save_requested){cfg=e.config;c77_engine_init(&e,&cfg,e.generation+1,true);}
    }return 0;
}
