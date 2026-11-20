#define SETTINGS_STORE_HOST_TEST
#include <assert.h>
#include <stdio.h>
#include "../src/settings_store.c"
static uint8_t memory[2*SECTOR_BYTES],backup[2*SECTOR_BYTES];
static int unlocked,layout=1,ready=1,fail_word=-1,programs,partial;
static unsigned erase_calls,last_erased;
static int flash_layout_valid(void){return layout;}
static int flash_ready(void){return ready;}
static int flash_read(uint32_t offset,uint8_t *out,uint32_t length){assert(layout&&offset+length<=sizeof memory);memcpy(out,memory+offset,length);return 1;}
static int flash_unlock(void){unlocked=1;return 1;}
static void flash_lock(void){unlocked=0;}
static int flash_erase(unsigned sector){assert(unlocked&&sector<2);erase_calls++;last_erased=sector;memset(memory+sector*SECTOR_BYTES,0xff,SECTOR_BYTES);return 1;}
static int flash_program(uint32_t offset,const uint8_t *word){assert(unlocked&&offset%32==0&&offset+32<=sizeof memory);for(unsigned i=0;i<32;i++)assert(memory[offset+i]==0xff);int failed=programs++==fail_word;unsigned n=failed?(unsigned)partial:32;memcpy(memory+offset,word,n);return !failed;}
/* What older firmware wrote: no custom curve after the 476-byte payload.
 * Format77 (<=0.3.42) holds thresholds in 0..1000; format78 (0.3.43-0.3.45). */
static void write_old(uint8_t *at,const c77_config *cfg,uint32_t generation,bool enabled,uint16_t version){
    uint8_t r[RECORD_BYTES];encode_record(r,cfg,generation,enabled);
    put16(r+4,version);put16(r+6,OLD_PAYLOAD_BYTES);memset(r+32+C77_CONFIG_BYTES,0,C77_CURVE_BYTES);
    const uint32_t crc=record_crc(r);put32(r+12,crc);
    make_commit(r+COMMIT_OFFSET,generation,crc);memcpy(at,r,sizeof r);
}
static void write_legacy(uint8_t *at,const c77_config *cfg,uint32_t generation,bool enabled)
{write_old(at,cfg,generation,enabled,LEGACY_FORMAT_VERSION);}
static void expect(uint32_t expected,uint16_t actuation,bool enabled){c77_config cfg;uint32_t gen;bool output;assert(settings_store_load(&cfg,&gen,&output));assert(gen==expected&&cfg.keys[0].actuation==actuation&&output==enabled);}
int main(void){
    memset(memory,0xff,sizeof memory);c77_config a,b;c77_defaults(&a);b=a;b.keys[0].actuation=3000;uint32_t gen=0;
    bool output=true;
    assert(!settings_store_load(&a,&gen,&output)&&!output);assert(settings_store_save(&a,&gen,false)&&gen==1);expect(1,2000,false);memcpy(backup,memory,sizeof memory);
    for(int word=0;word<(int)(RECORD_BYTES/32);word++)for(int cut=0;cut<=32;cut++){
        memcpy(memory,backup,sizeof memory);fail_word=word;partial=cut;programs=0;
        assert(!settings_store_save(&b,NULL,true)&&!unlocked);
        if(word==(int)(RECORD_BYTES/32)-1&&cut==32)expect(2,3000,true);else expect(1,2000,false);
    }
    memcpy(memory,backup,sizeof memory);fail_word=-1;programs=0;
    for(uint32_t i=0;i<RECORDS_PER_SECTOR;i++)encode_record(memory+i*RECORD_BYTES,&a,i+1,false);
    memcpy(backup,memory,sizeof memory);erase_calls=0;
    assert(settings_store_save(&b,&gen,true)&&gen==RECORDS_PER_SECTOR+1&&erase_calls==1&&last_erased==1);assert(!memcmp(memory,backup,SECTOR_BYTES));expect(gen,3000,true);
    uint8_t record[RECORD_BYTES],bad[RECORD_BYTES];encode_record(record,&a,33,false);
    for(unsigned i=0;i<sizeof record;i++)for(unsigned bit=0;bit<8;bit++){memcpy(bad,record,sizeof bad);bad[i]^=(uint8_t)(1u<<bit);assert(!decode_record(bad,NULL,NULL,NULL));}
    /* Old zero-reserved records stay valid; new bottom-zone values survive the
     * same journal format without rewriting prior generations. */
    c77_config migrated;uint32_t migrated_gen;assert(decode_record(record,&migrated,&migrated_gen,&output)&&!output);
    assert(migrated_gen==33&&migrated.keys[0].bottom_deadzone==0&&migrated.keys[0].curve_id==0);
    b.keys[0].bottom_deadzone=125;b.keys[0].curve_id=RT_CURVE_OWLAB_TI_REFERENCE;
    assert(settings_store_save(&b,&gen,false));
    assert(settings_store_load(&migrated,&migrated_gen,&output)&&!output&&migrated_gen==gen&&migrated.keys[0].bottom_deadzone==125&&migrated.keys[0].curve_id==1);
    /* New factory mappings must not replace any valid stored profile. */
    b.bindings[0]=(c77_binding){.kind=1,.code=0x39};
    b.bindings[13]=(c77_binding){.kind=1,.code=4,.modifiers=3};
    assert(settings_store_save(&b,&gen,false));
    c77_defaults(&migrated);assert(migrated.bindings[0].code==0x2b);
    assert(settings_store_load(&migrated,&migrated_gen,&output)&&!output&&migrated_gen==gen);
    uint8_t saved_blob[C77_CONFIG_BYTES],loaded_blob[C77_CONFIG_BYTES];
    c77_config_encode(&b,saved_blob);c77_config_encode(&migrated,loaded_blob);
    assert(!memcmp(saved_blob,loaded_blob,sizeof saved_blob));
    /* An update from firmware <=0.3.42 keeps the saved profile: format77
     * thresholds load x10, and the next save writes format78. */
    memset(memory,0xff,sizeof memory);
    c77_config old;c77_defaults(&old);
    for(unsigned i=0;i<C77_HALL;i++){
        old.keys[i].released=2194;old.keys[i].pressed=2625;old.keys[i].flags=7;old.keys[i].curve_id=1;
        old.keys[i].actuation=176;old.keys[i].rt_press=176;old.keys[i].rt_release=294;
        old.keys[i].top_deadzone=88;old.keys[i].bottom_deadzone=88;old.keys[i].noise=3;
    }
    write_legacy(memory,&old,13,true);
    assert(settings_store_load(&migrated,&migrated_gen,&output)&&output&&migrated_gen==13);
    for(unsigned i=0;i<C77_HALL;i++){
        const c77_key_config *k=&migrated.keys[i];
        assert(k->actuation==1760&&k->rt_press==1760&&k->rt_release==2940&&k->top_deadzone==880&&k->bottom_deadzone==880);
        assert(k->released==2194&&k->pressed==2625&&k->flags==7&&k->curve_id==1&&k->noise==3);
    }
    assert(settings_store_save(&migrated,&gen,true)&&gen==14);
    assert(get16(memory+RECORD_BYTES+4)==FORMAT_VERSION&&get16(memory+4)==LEGACY_FORMAT_VERSION);
    expect(14,1760,true);
    /* A format77 record outside the old 0..1000 range is not trusted. */
    old.keys[0].actuation=1500;write_legacy(memory+2*RECORD_BYTES,&old,15,false);
    expect(14,1760,true);
    /* Format78 (0.3.43-0.3.45) loads unchanged with the identity custom curve. */
    memset(memory,0xff,sizeof memory);c77_defaults(&old);old.keys[0].actuation=1760;old.keys[0].curve_id=1;
    write_old(memory,&old,20,true,FINE_FORMAT_VERSION);expect(20,1760,true);
    assert(settings_store_load(&migrated,&migrated_gen,&output)&&migrated.keys[0].curve_id==1);
    for(unsigned i=0;i<RT_CUSTOM_CURVE_POINTS;i++)assert(migrated.curve[i]==(i+1u)*625u);
    /* Format79 keeps a drawn curve and keys selecting it. */
    for(unsigned i=0;i<RT_CUSTOM_CURVE_POINTS;i++)migrated.curve[i]=(uint16_t)(i*i*40u);
    migrated.keys[4].curve_id=RT_CURVE_CUSTOM;
    assert(settings_store_save(&migrated,&gen,false)&&gen==21&&get16(memory+RECORD_BYTES+4)==FORMAT_VERSION);
    c77_config custom;assert(settings_store_load(&custom,&migrated_gen,&output)&&migrated_gen==21&&!output);
    assert(!memcmp(custom.curve,migrated.curve,sizeof custom.curve)&&custom.keys[4].curve_id==RT_CURVE_CUSTOM);
    assert(c77_config_crc(&custom)==c77_config_crc(&migrated));
    /* A non-monotonic stored curve is rejected; the older generation loads. */
    uint8_t *latest=memory+RECORD_BYTES;put16(latest+32+C77_CONFIG_BYTES+4,0); /* curve[2] < curve[1] */
    {const uint32_t crc=record_crc(latest);put32(latest+12,crc);make_commit(latest+COMMIT_OFFSET,21,crc);}
    expect(20,1760,true);
    layout=0;assert(!settings_store_load(&a,NULL,NULL)&&settings_store_last_error()==SETTINGS_ERROR_LAYOUT);assert(!settings_store_save(&b,NULL,false));layout=1;ready=0;assert(!settings_store_save(&b,NULL,false)&&settings_store_last_error()==SETTINGS_ERROR_CACHE);
    puts("settings: CRC/commit,torn-write cases,sector rotation,format77/78 migration,format79 custom curve,layout guard PASS");return 0;
}
