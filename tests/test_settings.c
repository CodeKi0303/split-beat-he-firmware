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
static void expect(uint32_t expected,uint16_t actuation,bool enabled){c77_config cfg;uint32_t gen;bool output;assert(settings_store_load(&cfg,&gen,&output));assert(gen==expected&&cfg.keys[0].actuation==actuation&&output==enabled);}
int main(void){
    memset(memory,0xff,sizeof memory);c77_config a,b;c77_defaults(&a);b=a;b.keys[0].actuation=300;uint32_t gen=0;
    bool output=true;
    assert(!settings_store_load(&a,&gen,&output)&&!output);assert(settings_store_save(&a,&gen,false)&&gen==1);expect(1,200,false);memcpy(backup,memory,sizeof memory);
    for(int word=0;word<(int)(RECORD_BYTES/32);word++)for(int cut=0;cut<=32;cut++){
        memcpy(memory,backup,sizeof memory);fail_word=word;partial=cut;programs=0;
        assert(!settings_store_save(&b,NULL,true)&&!unlocked);
        if(word==(int)(RECORD_BYTES/32)-1&&cut==32)expect(2,300,true);else expect(1,200,false);
    }
    memcpy(memory,backup,sizeof memory);fail_word=-1;programs=0;
    for(uint32_t i=0;i<RECORDS_PER_SECTOR;i++)encode_record(memory+i*RECORD_BYTES,&a,i+1,false);
    memcpy(backup,memory,sizeof memory);erase_calls=0;
    assert(settings_store_save(&b,&gen,true)&&gen==RECORDS_PER_SECTOR+1&&erase_calls==1&&last_erased==1);assert(!memcmp(memory,backup,SECTOR_BYTES));expect(gen,300,true);
    uint8_t record[RECORD_BYTES],bad[RECORD_BYTES];encode_record(record,&a,33,false);
    for(unsigned i=0;i<sizeof record;i++)for(unsigned bit=0;bit<8;bit++){memcpy(bad,record,sizeof bad);bad[i]^=(uint8_t)(1u<<bit);assert(!decode_record(bad,NULL,NULL,NULL));}
    /* Old zero-reserved records stay valid; new bottom-zone values survive the
     * same journal format without rewriting prior generations. */
    c77_config migrated;uint32_t migrated_gen;assert(decode_record(record,&migrated,&migrated_gen,&output)&&!output);
    assert(migrated_gen==33&&migrated.keys[0].bottom_deadzone==0&&migrated.keys[0].curve_id==0);
    b.keys[0].bottom_deadzone=125;b.keys[0].curve_id=RT_CURVE_OWLAB_TI_REFERENCE;
    assert(settings_store_save(&b,&gen,false));
    assert(settings_store_load(&migrated,&migrated_gen,&output)&&!output&&migrated_gen==gen&&migrated.keys[0].bottom_deadzone==125&&migrated.keys[0].curve_id==1);
    layout=0;assert(!settings_store_load(&a,NULL,NULL)&&settings_store_last_error()==SETTINGS_ERROR_LAYOUT);assert(!settings_store_save(&b,NULL,false));layout=1;ready=0;assert(!settings_store_save(&b,NULL,false)&&settings_store_last_error()==SETTINGS_ERROR_CACHE);
    puts("settings: CRC/commit,594 torn-write cases,sector rotation,layout guard PASS");return 0;
}
