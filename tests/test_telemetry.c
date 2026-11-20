/* Input telemetry ring, wire encoding, command handling and engine hooks.
 * Reuses the real engine fixture from test_core.c; no device access. */
#define main existing_core_tests
#include "test_core.c"
#undef main
#include "telemetry.h"

#if !C77_TELEMETRY
#error Build this test with HARDWARE_REVISION=86 ROLE_LEFT=1
#endif

typedef struct { uint32_t now, base, dropped; uint8_t count, flags; uint32_t time[8]; uint8_t kind[8], index[8]; uint16_t value[8]; } decoded;

static uint32_t g32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint16_t g16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}

static void decode(const uint8_t *p, decoded *d){
    memset(d,0,sizeof *d);
    d->now=g32(p);d->base=g32(p+4);d->dropped=g32(p+8);d->count=p[12];d->flags=p[13];
    uint32_t t=d->base;
    for(unsigned i=0;i<d->count;i++){
        const uint8_t *r=p+TELEMETRY_READ_HEADER_BYTES+i*TELEMETRY_RECORD_BYTES;
        t+=(uint32_t)(int32_t)(int16_t)g16(r);
        d->time[i]=t;d->kind[i]=(uint8_t)(r[2]>>6);d->index[i]=(uint8_t)(r[2]&0x3F);d->value[i]=g16(r+3);
    }
}

static decoded read_now(uint32_t at){uint8_t p[54];decoded d;(void)telemetry_pack(p,at);decode(p,&d);return d;}

static void test_sampling_policy(void){
    telemetry_arm(true,4,1000,0);
    telemetry_hall(3,2000,100,false);                 /* first sample of a key: always */
    telemetry_hall(3,2010,500,false);                 /* moved enough, but too soon */
    telemetry_hall(3,2002,1200,false);                /* old enough, too small a change */
    telemetry_hall(3,2010,1300,false);                /* both: recorded */
    telemetry_hall(3,2011,1301,true);                 /* forced at a decision */
    telemetry_hall(14,1,1400,true);                   /* out of range key: ignored */
    decoded d=read_now(5000);
    assert(d.count==3&&d.base==100&&d.now==5000&&d.dropped==0&&d.flags==1);
    assert(d.time[0]==100&&d.time[1]==1300&&d.time[2]==1301);
    assert(d.kind[0]==TELEMETRY_KIND_SAMPLE&&d.index[2]==3&&d.value[1]==2010&&d.value[2]==2011);
    assert(read_now(5001).count==0);
}

static void test_decisions_and_usb(void){
    telemetry_arm(true,4,1000,0);
    telemetry_decision(12,true,false,640,10);
    telemetry_decision(12,false,true,12000,20);        /* travel clamps to RT_TRAVEL_MAX (10000) */
    uint8_t before[32]={0},after[32]={0};
    after[0x04/8]|=1u<<(0x04%8);                       /* A pressed */
    after[0xE1/8]|=1u<<(0xE1%8);                       /* Left Shift pressed */
    telemetry_usb(before,after,30);
    telemetry_usb(after,after,40);                     /* no change: nothing */
    telemetry_usb(after,before,50);
    decoded d=read_now(60);
    assert(d.count==6);
    assert(d.kind[0]==TELEMETRY_KIND_DECISION&&d.index[0]==12&&d.value[0]==(0x8000|640));
    assert(d.value[1]==(0x4000|10000));
    assert(d.kind[2]==TELEMETRY_KIND_USB&&d.value[2]==(0x100|0x04)&&d.value[3]==(0x100|0xE1)&&d.time[3]==30);
    assert(d.value[4]==0x04&&d.value[5]==0xE1&&d.time[5]==50);
}

static void test_encoding_limits(void){
    telemetry_arm(true,1,0,0);
    telemetry_hall(0,100,1000,true);
    telemetry_hall(1,100,900,true);                    /* out of order: negative delta */
    telemetry_hall(2,100,900+32767,true);              /* largest forward delta */
    telemetry_hall(3,100,900+32767+32768,true);        /* too far: next reply */
    decoded d=read_now(200000);
    assert(d.count==3&&d.time[1]==900&&d.time[2]==900+32767&&(d.flags&2));
    d=read_now(200001);
    assert(d.count==1&&d.base==900+32767+32768&&d.index[0]==3&&!(d.flags&2));
    for(unsigned i=0;i<20;i++)telemetry_hall(i%14,(uint16_t)i,5000+i,true);
    d=read_now(9000);assert(d.count==TELEMETRY_RECORDS_PER_READ&&(d.flags&2));
    /* A full ring counts losses instead of overwriting unread records. */
    telemetry_arm(true,1,0,0);
    for(uint32_t i=0;i<TELEMETRY_CAPACITY+5;i++)telemetry_hall(i%14,0,i,true);
    telemetry_status s;telemetry_get_status(&s);
    assert(s.pending==TELEMETRY_CAPACITY&&s.dropped==5);
    d=read_now(0);assert(d.time[0]==0&&d.dropped==5);
}

static void request(uint8_t op,const uint8_t *payload,unsigned len,uint8_t reply[64],uint32_t at){
    uint8_t q[64];packet(q,op,payload,len);
    assert(telemetry_command(q,reply,at));
    assert(c77_crc16(reply,62)==g16(reply+62)&&reply[3]==(op|0x80));
}

static void test_telemetry_commands(void){
    uint8_t r[64];
    telemetry_arm(false,0,0,0);
    request(TELEMETRY_CONTROL_COMMAND,NULL,0,r,10);
    assert(r[6]==C77_OK&&r[7]==20&&r[8]==0&&g32(r+12)==TELEMETRY_CAPACITY&&g32(r+16)==10);
    request(TELEMETRY_READ_COMMAND,NULL,0,r,11);assert(r[6]==C77_NOT_READY);
    const uint8_t bad_interval[4]={1,4,0x11,0x27};     /* 10001 us */
    request(TELEMETRY_CONTROL_COMMAND,bad_interval,4,r,12);assert(r[6]==C77_BAD_CONFIG&&!telemetry_armed());
    const uint8_t no_threshold[4]={1,0,0xE8,0x03};
    request(TELEMETRY_CONTROL_COMMAND,no_threshold,4,r,12);assert(r[6]==C77_BAD_CONFIG);
    const uint8_t arm[4]={1,6,0xF4,0x01};              /* threshold 6, 500 us */
    request(TELEMETRY_CONTROL_COMMAND,arm,4,r,100);
    assert(r[6]==C77_OK&&r[8]==1&&r[9]==6&&g16(r+10)==500&&telemetry_armed());
    telemetry_hall(5,1234,150,false);
    request(TELEMETRY_READ_COMMAND,NULL,0,r,200);
    decoded d;decode(r+8,&d);
    assert(r[6]==C77_OK&&r[7]==TELEMETRY_READ_HEADER_BYTES+TELEMETRY_RECORD_BYTES&&d.count==1&&d.value[0]==1234&&d.now==200);
    const uint8_t extra=1;request(TELEMETRY_READ_COMMAND,&extra,1,r,201);assert(r[6]==C77_BAD_PACKET);
    /* Reads keep it armed; silence disarms it and drops what was buffered. */
    telemetry_poll(200+TELEMETRY_IDLE_DISARM_US);assert(telemetry_armed());
    telemetry_poll(201+TELEMETRY_IDLE_DISARM_US);assert(!telemetry_armed());
    const uint8_t off[4]={0,0,0,0};request(TELEMETRY_CONTROL_COMMAND,off,4,r,300);assert(r[6]==C77_OK&&r[8]==0);
    uint8_t other[64];packet(other,0x40,NULL,0);assert(!telemetry_command(other,r,0));
}

static void test_engine_hooks(void){
    c77_config c;c77_defaults(&c);
    for(unsigned i=0;i<C77_HALL;i++){c.keys[i].released=1000;c.keys[i].pressed=2000;c.keys[i].flags=7;c.keys[i].noise=0;}
    c77_engine_init(&e,&c,0,false);now=100;fresh(1000);enable();
    telemetry_arm(true,1,0,now);
    const unsigned travel[]={0,100,300,600,900,700,400,100};
    for(unsigned i=0;i<sizeof travel/sizeof travel[0];i++){
        c77_sample_t s=e.sample[1];s.sequence++;now+=250;s.timestamp_us=now;s.raw[2]=(uint16_t)(1000+travel[i]);
        c77_update(&e,1,&s,now);                          /* right side, key 9 */
    }
    unsigned samples=0,presses=0,releases=0;uint8_t p[54];decoded d;
    while(telemetry_pack(p,now),decode(p,&d),d.count){
        for(unsigned i=0;i<d.count;i++){
            if(d.index[i]!=9)continue;
            if(d.kind[i]==TELEMETRY_KIND_SAMPLE)samples++;
            if(d.kind[i]==TELEMETRY_KIND_DECISION){if(d.value[i]&0x8000)presses++;else releases++;}
        }
    }
    assert(samples>=7&&presses==1&&releases==1);
    telemetry_arm(false,0,0,now);
}

int main(void){
    test_sampling_policy();
    test_decisions_and_usb();
    test_encoding_limits();
    test_telemetry_commands();
    test_engine_hooks();
    puts("telemetry tests passed");
    return 0;
}
