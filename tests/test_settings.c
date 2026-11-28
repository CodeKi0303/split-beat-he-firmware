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
/* What older firmware wrote: 576-byte records. Format77 (<=0.3.42) holds
 * thresholds in 0..1000, format78 (0.3.43-0.3.45) has no custom curve and
 * format79 (0.3.46-0.3.49) appends the curve. */
static void write_old(uint8_t *at,const c77_config *cfg,uint32_t generation,bool enabled,uint16_t version){
    uint8_t r[OLD_RECORD_BYTES];memset(r,0,sizeof r);
    put32(r,RECORD_MAGIC);put16(r+4,version);put16(r+6,version==CURVE_FORMAT_VERSION?CURVE_PAYLOAD_BYTES:OLD_PAYLOAD_BYTES);
    put32(r+8,generation);r[16]=enabled;c77_config_encode(cfg,r+32);
    if(version==CURVE_FORMAT_VERSION)c77_curve_encode(cfg->curve,r+32+C77_CONFIG_BYTES);
    const uint32_t crc=record_crc(r,OLD_COMMIT_OFFSET);put32(r+12,crc);
    make_commit(r+OLD_COMMIT_OFFSET,generation,crc);memcpy(at,r,sizeof r);
}
static void reseal(uint8_t *r){const uint32_t crc=record_crc(r,COMMIT_OFFSET);put32(r+12,crc);make_commit(r+COMMIT_OFFSET,get32(r+8),crc);}
static void expect(uint32_t expected,uint16_t actuation,bool enabled){c77_config cfg;uint32_t gen;bool output;assert(settings_store_load(&cfg,NULL,&gen,&output));assert(gen==expected&&cfg.keys[0].actuation==actuation&&output==enabled);}
static void calibrate(c77_config *c){for(unsigned i=0;i<C77_HALL;i++){c->keys[i].released=2194;c->keys[i].pressed=2625;c->keys[i].flags=7;}}
int main(void){
    memset(memory,0xff,sizeof memory);c77_config a,b;c77_defaults(&a);b=a;b.keys[0].actuation=3000;uint32_t gen=0;
    bool output=true;
    assert(!settings_store_load(&a,NULL,&gen,&output)&&!output);assert(settings_store_save(&a,NULL,&gen,false)&&gen==1);expect(1,2000,false);
    assert(get16(memory+4)==FORMAT_VERSION);memcpy(backup,memory,sizeof memory);
    /* Torn writes: until the commit flashword lands, the older generation wins. */
    for(int word=0;word<(int)(RECORD_BYTES/32);word++)for(int cut=0;cut<=32;cut+=8){
        memcpy(memory,backup,sizeof memory);fail_word=word;partial=cut;programs=0;
        assert(!settings_store_save(&b,NULL,NULL,true)&&!unlocked);
        if(word==(int)(RECORD_BYTES/32)-1&&cut==32)expect(2,3000,true);else expect(1,2000,false);
    }
    /* A full sector rotates into the other one and leaves the full one intact. */
    memcpy(memory,backup,sizeof memory);fail_word=-1;programs=0;
    for(uint32_t i=0;i<RECORDS_PER_SECTOR;i++)encode_record(memory+i*RECORD_BYTES,&a,NULL,i+1,false);
    memcpy(backup,memory,sizeof memory);erase_calls=0;
    assert(settings_store_save(&b,NULL,&gen,true)&&gen==RECORDS_PER_SECTOR+1&&erase_calls==1&&last_erased==1);assert(!memcmp(memory,backup,SECTOR_BYTES));expect(gen,3000,true);
    /* Every single-bit flip of a record is rejected. */
    static uint8_t record[RECORD_BYTES],bad[RECORD_BYTES];encode_record(record,&a,NULL,33,false);
    for(unsigned i=0;i<sizeof record;i++)for(unsigned bit=0;bit<8;bit++){memcpy(bad,record,sizeof bad);bad[i]^=(uint8_t)(1u<<bit);assert(!decode_record(bad,RECORD_BYTES,NULL,NULL,NULL,NULL));}
    assert(!decode_record(record,OLD_RECORD_BYTES,NULL,NULL,NULL,NULL));
    c77_config migrated;c77_profile_bank bank;uint32_t migrated_gen;
    assert(decode_record(record,RECORD_BYTES,&migrated,&bank,&migrated_gen,&output)&&!output&&migrated_gen==33);
    assert(bank.used==1&&bank.active==0);
    /* Bottom zone, curve id and new factory mappings survive save/load. */
    b.keys[0].bottom_deadzone=125;b.keys[0].curve_id=RT_CURVE_OWLAB_TI_REFERENCE;
    b.bindings[0]=(c77_binding){.kind=1,.code=0x39};
    b.bindings[13]=(c77_binding){.kind=1,.code=4,.modifiers=3};
    assert(settings_store_save(&b,NULL,&gen,false));
    c77_defaults(&migrated);assert(migrated.bindings[0].code==0x2b);
    assert(settings_store_load(&migrated,NULL,&migrated_gen,&output)&&!output&&migrated_gen==gen);
    uint8_t saved_blob[C77_CONFIG_BYTES],loaded_blob[C77_CONFIG_BYTES];
    c77_config_encode(&b,saved_blob);c77_config_encode(&migrated,loaded_blob);
    assert(!memcmp(saved_blob,loaded_blob,sizeof saved_blob));

    /* Four profiles: per-profile thresholds, flags, bindings and names round-trip;
     * the active slot is refreshed from the live configuration on save. */
    memset(memory,0xff,sizeof memory);
    c77_config live;c77_defaults(&live);calibrate(&live);
    c77_profile_bank profiles;c77_profiles_init(&profiles,&live);
    memcpy(profiles.slots[0].name,"Game",4);
    profiles.slots[1]=profiles.slots[0];memset(profiles.slots[1].name,0,C77_PROFILE_NAME_BYTES);
    for(unsigned i=0;i<C77_HALL;i++){profiles.slots[1].keys[i].actuation=900;profiles.slots[1].keys[i].rt_press=150;profiles.slots[1].keys[i].flags=0;} /* RT off in profile 2 */
    profiles.slots[1].bindings[4]=(c77_binding){.kind=7,.code=1200,.modifiers=0};
    profiles.slots[3]=profiles.slots[0];memcpy(profiles.slots[3].name,"\xed\x83\x80\xec\x9d\xb4\xed\x95\x91",9); /* UTF-8 */
    profiles.slots[3].bindings[0]=(c77_binding){.kind=0};
    profiles.used=0x0b;profiles.active=1;
    assert(c77_profile_apply(&live,&profiles.slots[1]));
    live.keys[2].actuation=777; /* edited after the switch, not yet captured */
    assert(c77_profiles_valid(&profiles,&live));
    assert(settings_store_save(&live,&profiles,&gen,true)&&gen==1);
    c77_profile_bank loaded;c77_config loaded_live;
    assert(settings_store_load(&loaded_live,&loaded,&gen,&output)&&gen==1&&output);
    assert(loaded.used==0x0b&&loaded.active==1&&loaded_live.keys[2].actuation==777&&loaded_live.keys[0].actuation==900);
    assert(loaded.slots[1].keys[2].actuation==777&&loaded.slots[1].bindings[4].kind==7);
    assert(!memcmp(loaded.slots[0].name,"Game",5)&&loaded.slots[3].name[0]==0xed&&loaded.slots[3].bindings[0].kind==0);
    {static uint8_t want[C77_PROFILE_BANK_BYTES],got[C77_PROFILE_BANK_BYTES];c77_profile_bank expected_bank=profiles;
     c77_profile_capture(&expected_bank.slots[1],&live);c77_profiles_encode(&expected_bank,want);c77_profiles_encode(&loaded,got);
     assert(!memcmp(want,got,sizeof want));
     for(unsigned i=0;i<C77_PROFILE_BYTES;i++)assert(got[2+2*C77_PROFILE_BYTES+i]==0);}
    /* Calibration stays shared: profile 0 applied over the loaded live config keeps it. */
    c77_config other=loaded_live;assert(c77_profile_apply(&other,&loaded.slots[0]));
    assert(other.keys[0].released==2194&&other.keys[0].actuation==2000&&(other.keys[0].flags&2)&&(other.keys[0].flags&4)&&!(loaded_live.keys[0].flags&2)&&(loaded_live.keys[0].flags&4));
    /* Invalid banks are refused before any flash access. */
    c77_profile_bank broken=profiles;broken.active=2;erase_calls=0;
    assert(!settings_store_save(&live,&broken,NULL,false)&&settings_store_last_error()==SETTINGS_ERROR_ARGUMENT&&erase_calls==0);
    broken=profiles;broken.slots[3].name[0]=0x07;assert(!c77_profiles_valid(&broken,&live));
    broken=profiles;broken.slots[3].name[0]=0;assert(!c77_profiles_valid(&broken,&live)); /* NUL then text */
    broken=profiles;broken.slots[2].keys[0].actuation=1;assert(!c77_profiles_valid(&broken,&live)); /* unused slot not empty */
    broken=profiles;broken.slots[0].keys[0].flags=4;assert(!c77_profiles_valid(&broken,&live)); /* shared flag in a profile */
    broken=profiles;broken.slots[3].keys[0].top_deadzone=3000;assert(!c77_profiles_valid(&broken,&live));
    /* A stored record whose active slot disagrees with the live keys is rejected. */
    memcpy(record,memory,RECORD_BYTES);
    put16(record+32+CURVE_PAYLOAD_BYTES+2+1*C77_PROFILE_BYTES+2*11+1,778);reseal(record);
    assert(!decode_record(record,RECORD_BYTES,NULL,NULL,NULL,NULL));

    /* Format 80 has the same record stride and the original bank prefix.
     * Loading all four slots must preserve their data and select coupled release. */
    put16(memory+4,PROFILE_FORMAT_VERSION);
    put16(memory+6,CURVE_PAYLOAD_BYTES+C77_V80_BANK_BYTES);
    reseal(memory);
    assert(settings_store_load(&loaded_live,&loaded,&gen,&output)&&gen==1&&output);
    assert(loaded.used==0x0b&&loaded.active==1&&loaded.slots[1].keys[2].actuation==777);
    assert(loaded_live.keys[2].fixed_release==0 && !memcmp(loaded.slots[0].name,"Game",5));
    loaded_live.keys[2].fixed_release=443;
    loaded.slots[3].keys[4].fixed_release=1234;
    assert(settings_store_save(&loaded_live,&loaded,&gen,true)&&gen==2);
    assert(get16(memory+RECORD_BYTES+4)==FORMAT_VERSION);
    assert(settings_store_load(&loaded_live,&loaded,&gen,NULL)&&gen==2);
    assert(loaded_live.keys[2].fixed_release==443 && loaded.slots[1].keys[2].fixed_release==443);
    assert(c77_profile_apply(&loaded_live,&loaded.slots[3]));
    assert(loaded_live.keys[2].fixed_release==0 && loaded_live.keys[4].fixed_release==1234);
    /* A CRC-correct record with a different release in its active slot is invalid. */
    memcpy(record,memory+RECORD_BYTES,RECORD_BYTES);
    put16(record+32+CURVE_PAYLOAD_BYTES+C77_V80_BANK_BYTES+2*(C77_HALL+2),444);
    reseal(record);
    assert(!decode_record(record,RECORD_BYTES,NULL,NULL,NULL,NULL));

    /* An update from firmware <=0.3.42 keeps the saved profile: format77
     * thresholds load x10. The first format80 save goes to a freshly erased
     * other sector, so the 576-byte records stay readable by older firmware. */
    memset(memory,0xff,sizeof memory);
    c77_config old;c77_defaults(&old);
    for(unsigned i=0;i<C77_HALL;i++){
        old.keys[i].released=2194;old.keys[i].pressed=2625;old.keys[i].flags=7;old.keys[i].curve_id=1;
        old.keys[i].actuation=176;old.keys[i].rt_press=176;old.keys[i].rt_release=294;
        old.keys[i].top_deadzone=88;old.keys[i].bottom_deadzone=88;old.keys[i].noise=3;
    }
    write_old(memory,&old,13,true,LEGACY_FORMAT_VERSION);
    assert(settings_store_load(&migrated,&bank,&migrated_gen,&output)&&output&&migrated_gen==13&&bank.used==1&&bank.active==0);
    for(unsigned i=0;i<C77_HALL;i++){
        const c77_key_config *k=&migrated.keys[i];
        assert(k->actuation==1760&&k->rt_press==1760&&k->rt_release==2940&&k->top_deadzone==880&&k->bottom_deadzone==880);
        assert(k->released==2194&&k->pressed==2625&&k->flags==7&&k->curve_id==1&&k->noise==3);
        assert(bank.slots[0].keys[i].actuation==1760&&bank.slots[0].keys[i].flags==2);
    }
    memcpy(backup,memory,SECTOR_BYTES);erase_calls=0;
    assert(settings_store_save(&migrated,&bank,&gen,true)&&gen==14&&erase_calls==1&&last_erased==1);
    assert(get16(memory+SECTOR_BYTES+4)==FORMAT_VERSION&&get16(memory+4)==LEGACY_FORMAT_VERSION&&!memcmp(memory,backup,SECTOR_BYTES));
    expect(14,1760,true);
    /* The next save appends in the format80 sector without erasing. */
    erase_calls=0;assert(settings_store_save(&migrated,&bank,&gen,true)&&gen==15&&erase_calls==0&&get16(memory+SECTOR_BYTES+RECORD_BYTES+4)==FORMAT_VERSION);
    /* A format77 record outside the old 0..1000 range is not trusted. */
    memset(memory,0xff,sizeof memory);write_old(memory,&old,13,true,LEGACY_FORMAT_VERSION);
    old.keys[0].actuation=1500;write_old(memory+OLD_RECORD_BYTES,&old,15,false,LEGACY_FORMAT_VERSION);
    expect(13,1760,true);
    /* Format78 (0.3.43-0.3.45) loads unchanged with the identity custom curve. */
    memset(memory,0xff,sizeof memory);c77_defaults(&old);old.keys[0].actuation=1760;old.keys[0].curve_id=1;
    write_old(memory,&old,20,true,FINE_FORMAT_VERSION);expect(20,1760,true);
    assert(settings_store_load(&migrated,NULL,&migrated_gen,&output)&&migrated.keys[0].curve_id==1);
    for(unsigned i=0;i<RT_CUSTOM_CURVE_POINTS;i++)assert(migrated.curve[i]==(i+1u)*625u);
    /* Format79 (0.3.46-0.3.49) keeps a drawn curve and the keys selecting it. */
    for(unsigned i=0;i<RT_CUSTOM_CURVE_POINTS;i++)migrated.curve[i]=(uint16_t)(i*i*40u);
    migrated.keys[4].curve_id=RT_CURVE_CUSTOM;
    write_old(memory+OLD_RECORD_BYTES,&migrated,21,false,CURVE_FORMAT_VERSION);
    c77_config custom;assert(settings_store_load(&custom,&bank,&migrated_gen,&output)&&migrated_gen==21&&!output&&bank.used==1);
    assert(!memcmp(custom.curve,migrated.curve,sizeof custom.curve)&&custom.keys[4].curve_id==RT_CURVE_CUSTOM);
    assert(c77_config_crc(&custom)==c77_config_crc(&migrated));
    assert(settings_store_save(&custom,&bank,&gen,false)&&gen==22);
    c77_config again;assert(settings_store_load(&again,NULL,NULL,NULL)&&!memcmp(again.curve,custom.curve,sizeof again.curve));
    /* A non-monotonic stored curve is rejected; the older generation loads. */
    uint8_t *latest=memory+SECTOR_BYTES;put16(latest+32+C77_CONFIG_BYTES+4,0); /* curve[2] < curve[1] */
    reseal(latest);
    assert(settings_store_load(&again,NULL,&migrated_gen,NULL)&&migrated_gen==21);
    layout=0;assert(!settings_store_load(&a,NULL,NULL,NULL)&&settings_store_last_error()==SETTINGS_ERROR_LAYOUT);assert(!settings_store_save(&b,NULL,NULL,false));layout=1;ready=0;assert(!settings_store_save(&b,NULL,NULL,false)&&settings_store_last_error()==SETTINGS_ERROR_CACHE);
    puts("settings: CRC/commit,torn-write cases,sector rotation,formats 77..80 migration,per-profile fixed release,layout guard PASS");return 0;
}
