/* Offline update safety tests. No USB, programmer, or hardware access. */
#include "fw_update.h"
#include "fw_update_storage.h"
#include "fw_update_flash_mock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_BYTES 0x100000U
#define SECTOR 0x20000U
#define SLOT_OFFSET 0x20000U
#define IMAGE_BYTES 2048U
#if ROLE_LEFT
#define ROLE 0U
#else
#define ROLE 1U
#endif
static uint8_t flash_memory[FLASH_BYTES], ecc_bad[FLASH_BYTES/32U];
static uint8_t boot_canary[SECTOR], image[IMAGE_BYTES], packet[64], reply[64];
static uint8_t protected_canary[FLASH_BYTES-3U*SECTOR];
static const uint8_t target_uid[12]={1,3,5,7,9,11,13,15,17,19,21,23};
static unsigned checks,erases,programs,erase_order[2],fail_erase,fail_program;
static unsigned erase_cut,program_cut;
static uint16_t physical_kib=1024;
static bool runtime_ready=true;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static void u16(uint8_t *p,uint16_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }
static void u32(uint8_t *p,uint32_t n) { for (unsigned i=0;i<4U;i++) p[i]=(uint8_t)(n>>(8U*i)); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
uint16_t fw_update_mock_kib(void) { return physical_kib; }
void fw_update_mock_uid(uint8_t uid[12]) { memcpy(uid,target_uid,12); }
bool fw_update_mock_ready(void) { return runtime_ready; }
bool fw_update_mock_read(uint32_t address,uint8_t *out,uint32_t length)
{
    if (address<FW_UPDATE_BOOT_BASE || length>FLASH_BYTES || address-FW_UPDATE_BOOT_BASE>FLASH_BYTES-length) return false;
    uint32_t offset=address-FW_UPDATE_BOOT_BASE;
    for (uint32_t at=offset/32U;at<=(offset+length-1U)/32U;at++) if (ecc_bad[at]) return false;
    memcpy(out,flash_memory+offset,length); return true;
}
bool fw_update_mock_erase(unsigned sector)
{
    CHECK(sector==1U || sector==2U);
    if (erases<2U) erase_order[erases]=sector;
    erases++;
    unsigned bytes=(erases==fail_erase)?erase_cut:SECTOR;
    CHECK(bytes<=SECTOR);
    memset(flash_memory+sector*SECTOR,0xff,bytes);
    memset(ecc_bad+sector*SECTOR/32U,0,bytes/32U);
    return erases!=fail_erase;
}
bool fw_update_mock_program(uint32_t address,const uint8_t word[32])
{
    CHECK(address>=FW_UPDATE_APP_BASE && address<=FW_UPDATE_SLOT_END-32U && !(address&31U));
    programs++;
    unsigned bytes=(programs==fail_program)?program_cut:32U;
    CHECK(bytes<=32U);
    uint32_t offset=address-FW_UPDATE_BOOT_BASE;
    for (unsigned i=0;i<32U;i++) CHECK(flash_memory[offset+i]==0xffU);
    memcpy(flash_memory+offset,word,bytes);
    if (bytes>0U && bytes<32U) ecc_bad[offset/32U]=1;
    return programs!=fail_program;
}
static void make_identity(uint8_t *out,bool boot,uint8_t role,uint32_t version)
{
    memset(out,0,32); memcpy(out,boot?"C86BL003":"C86FW003",8);
    out[8]=86; out[9]=role; u16(out+10,1); u32(out+12,version);
    u32(out+16,FW_UPDATE_APP_BASE); u32(out+20,FW_UPDATE_MAX_IMAGE);
    u32(out+24,FW_UPDATE_IDENTITY_MAGIC); u32(out+28,(uint32_t)~FW_UPDATE_IDENTITY_MAGIC);
}
static void make_image(void)
{
    for (unsigned i=0;i<IMAGE_BYTES;i++) image[i]=(uint8_t)(i*13U+0x42U);
    u32(image,0x20020000U); u32(image+4,FW_UPDATE_APP_BASE+0x501U);
    make_identity(image+FW_UPDATE_IDENTITY_OFFSET,false,ROLE,FW_UPDATE_VERSION);
}
static void make_metadata(uint8_t *record)
{
    u32(record,FW_UPDATE_METADATA_MAGIC); u32(record+4,IMAGE_BYTES);
    u32(record+8,fw_update_crc32(image,IMAGE_BYTES)); u32(record+12,FW_UPDATE_VERSION);
    u32(record+16,86U|(ROLE<<8)|(1U<<16)); u32(record+20,FW_UPDATE_APP_BASE);
    u32(record+24,fw_update_crc32(record,24)); u32(record+28,(uint32_t)~FW_UPDATE_METADATA_MAGIC);
}
static void reset_fixture(bool old_image)
{
    memset(flash_memory,0xa5,sizeof flash_memory); memset(ecc_bad,0,sizeof ecc_bad);
    memset(protected_canary,0xa5,sizeof protected_canary);
    memset(flash_memory+SLOT_OFFSET,0xff,2U*SECTOR);
    make_identity(flash_memory+FW_UPDATE_IDENTITY_OFFSET,true,ROLE,FW_UPDATE_BOOT_VERSION);
    memcpy(boot_canary,flash_memory,SECTOR); make_image();
    if (old_image) {
        memcpy(flash_memory+SLOT_OFFSET,image,IMAGE_BYTES);
        make_metadata(flash_memory+(FW_UPDATE_METADATA_BASE-FW_UPDATE_BOOT_BASE));
    }
    erases=programs=fail_erase=fail_program=erase_cut=program_cut=0;
    memset(erase_order,0,sizeof erase_order); physical_kib=1024; runtime_ready=true;
    fw_update_init();
}
static void canaries(void)
{
    CHECK(!memcmp(flash_memory,boot_canary,SECTOR));
    CHECK(!memcmp(flash_memory+3U*SECTOR,protected_canary,sizeof protected_canary));
}
static void seal(void) { u16(packet+62,fw_update_crc16(packet,62)); }
static void request(uint8_t op,const uint8_t *data,uint8_t size)
{
    memset(packet,0,64); packet[0]='C'; packet[1]='7'; packet[2]=1; packet[3]=op;
    u16(packet+4,0x1234); packet[7]=size;
    if (size) memcpy(packet+8,data,size);
    seal();
}
static uint8_t dispatch(uint8_t mode)
{
    CHECK(fw_update_handle(packet,reply,mode));
    CHECK(reply[0]=='C' && reply[1]=='7' && reply[2]==1 && reply[3]==(uint8_t)(packet[3]|0x80U));
    CHECK(reply[4]==packet[4] && reply[5]==packet[5]);
    CHECK(fw_update_crc16(reply,62)==(uint16_t)((uint16_t)reply[62]|((uint16_t)reply[63]<<8)));
    CHECK(reply[7]<=54U);
    for (unsigned i=8U+reply[7];i<62U;i++) CHECK(!reply[i]);
    if (reply[6]) CHECK(!reply[7]);
    return reply[6];
}
static uint8_t command(uint8_t mode,uint8_t op,const uint8_t *data,uint8_t size)
{ request(op,data,size); return dispatch(mode); }
static void manifest(uint8_t out[32])
{
    memset(out,0,32); memcpy(out,target_uid,12); out[12]=86; out[13]=ROLE;
    u32(out+16,0x12345678U); u32(out+20,IMAGE_BYTES);
    u32(out+24,fw_update_crc32(image,IMAGE_BYTES)); u32(out+28,FW_UPDATE_VERSION);
}
static void common_tests(void)
{
    reset_fixture(true);
    CHECK(fw_update_crc16((const uint8_t *)"123456789",9)==0x29b1U);
    CHECK(fw_update_crc32((const uint8_t *)"123456789",9)==0xcbf43926U);
    CHECK(fw_update_boot_present()==(HARDWARE_REVISION==86));
    CHECK(fw_update_app_valid(NULL,NULL)==(HARDWARE_REVISION==86));
    CHECK(command(FW_UPDATE_MODE_APP,FW_UPDATE_INFO,NULL,0)==FW_UPDATE_OK);
    CHECK(reply[7]==48 && reply[8+1]==HARDWARE_REVISION && reply[8+2]==ROLE);
    CHECK(reply[8+6]==(HARDWARE_REVISION==86?3U:0U));
    CHECK(r32(reply+8+16)==(HARDWARE_REVISION==86?IMAGE_BYTES:0U));
    CHECK(!memcmp(reply+8+36,target_uid,12));
    CHECK(command(FW_UPDATE_MODE_APP,FW_UPDATE_ENTER,(const uint8_t *)"UPD3",4)==
          (HARDWARE_REVISION==86?FW_UPDATE_OK:FW_UPDATE_NOT_READY));
    CHECK(fw_update_take_action()==(HARDWARE_REVISION==86?FW_UPDATE_ACTION_ENTER:FW_UPDATE_ACTION_NONE));
    CHECK(fw_update_take_action()==FW_UPDATE_ACTION_NONE);
    CHECK(command(FW_UPDATE_MODE_APP,FW_UPDATE_ENTER,(const uint8_t *)"UPD3",4)==
          (HARDWARE_REVISION==86?FW_UPDATE_OK:FW_UPDATE_NOT_READY));
    fw_update_cancel_action(); CHECK(fw_update_take_action()==FW_UPDATE_ACTION_NONE);
    flash_memory[FW_UPDATE_IDENTITY_OFFSET+9]^=1U;
    CHECK(command(FW_UPDATE_MODE_APP,FW_UPDATE_ENTER,(const uint8_t *)"UPD3",4)==FW_UPDATE_NOT_READY);
    CHECK(fw_update_take_action()==FW_UPDATE_ACTION_NONE);
    reset_fixture(true);
    CHECK(command(FW_UPDATE_MODE_APP,FW_UPDATE_ENTER,(const uint8_t *)"NOPE",4)==FW_UPDATE_BAD_CONFIG);
    uint8_t m[32]; manifest(m);
    CHECK(command(FW_UPDATE_MODE_APP,FW_UPDATE_BEGIN,m,32)==FW_UPDATE_NOT_READY);
    CHECK(!erases && !programs);
    request(FW_UPDATE_INFO,NULL,0); packet[2]=2; seal(); CHECK(dispatch(FW_UPDATE_MODE_APP)==FW_UPDATE_BAD_PACKET);
    request(FW_UPDATE_INFO,NULL,0); packet[4]=packet[5]=0; seal(); CHECK(dispatch(FW_UPDATE_MODE_APP)==FW_UPDATE_BAD_PACKET);
    request(FW_UPDATE_INFO,NULL,0); packet[6]=1; seal(); CHECK(dispatch(FW_UPDATE_MODE_APP)==FW_UPDATE_BAD_PACKET);
    request(FW_UPDATE_INFO,NULL,0); packet[7]=55; seal(); CHECK(dispatch(FW_UPDATE_MODE_APP)==FW_UPDATE_BAD_PACKET);
    request(FW_UPDATE_INFO,NULL,0); packet[61]=1; seal(); CHECK(dispatch(FW_UPDATE_MODE_APP)==FW_UPDATE_BAD_PACKET);
    request(FW_UPDATE_INFO,NULL,0); packet[62]^=1; CHECK(dispatch(FW_UPDATE_MODE_APP)==FW_UPDATE_BAD_PACKET);
    request(FW_UPDATE_INFO,NULL,0); packet[7]=1; seal(); CHECK(dispatch(FW_UPDATE_MODE_APP)==FW_UPDATE_BAD_PACKET);
    request(FW_UPDATE_INFO,NULL,0); CHECK(dispatch(2)==FW_UPDATE_NOT_READY);
    packet[0]='X'; CHECK(!fw_update_handle(packet,reply,FW_UPDATE_MODE_APP));
    CHECK(!fw_update_handle(NULL,reply,FW_UPDATE_MODE_APP));
    request(FW_UPDATE_INFO,NULL,0); CHECK(!fw_update_handle(packet,NULL,FW_UPDATE_MODE_APP));
    packet[3]=0x37; CHECK(!fw_update_handle(packet,reply,FW_UPDATE_MODE_APP));
    uint8_t data[256];
    CHECK(!fw_update_flash_read(UINT32_MAX,data,1)); CHECK(!fw_update_flash_read(0,data,257));
    CHECK(!fw_update_flash_read(0x3ffffU,data,2)); CHECK(!fw_update_flash_read(0,data,0));
    CHECK(!fw_update_flash_read(0,NULL,1));
    physical_kib=512; CHECK(!fw_update_boot_present()); CHECK(!fw_update_flash_read(0,data,1));
    CHECK(!fw_update_app_valid(NULL,NULL)); physical_kib=1024;
    ecc_bad[FW_UPDATE_IDENTITY_OFFSET/32U]=1; CHECK(!fw_update_boot_present());
    ecc_bad[FW_UPDATE_IDENTITY_OFFSET/32U]=0;
    ecc_bad[(FW_UPDATE_METADATA_BASE-FW_UPDATE_BOOT_BASE)/32U]=1;
    CHECK(!fw_update_app_valid(NULL,NULL));
    canaries();
}

#if FW_UPDATE_BOOT && HARDWARE_REVISION==86
static uint8_t start(void)
{ uint8_t m[32]; manifest(m); return command(FW_UPDATE_MODE_BOOT,FW_UPDATE_BEGIN,m,32); }
static uint8_t chunk(uint32_t offset)
{
    uint8_t data[40]; u32(data,0x12345678U); u32(data+4,offset);
    memcpy(data+8,image+offset,32);
    return command(FW_UPDATE_MODE_BOOT,FW_UPDATE_WRITE,data,40);
}
static uint8_t finish(void)
{ uint8_t data[4]; u32(data,0x12345678U); return command(FW_UPDATE_MODE_BOOT,FW_UPDATE_COMMIT,data,4); }
static void transfer_image(void)
{ for (uint32_t offset=0;offset<IMAGE_BYTES;offset+=32U) CHECK(chunk(offset)==FW_UPDATE_OK); }
static void happy_and_bounds(void)
{
    reset_fixture(true); image[1536]^=1U;
    CHECK(start()==FW_UPDATE_OK && erases==2U && erase_order[0]==2U && erase_order[1]==1U);
    CHECK(!fw_update_app_valid(NULL,NULL)); CHECK(start()==FW_UPDATE_OK && erases==2U);
    uint8_t m[32]; manifest(m); m[24]^=1;
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_BEGIN,m,32)==FW_UPDATE_BUSY);
    manifest(m); m[16]^=1;
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_BEGIN,m,32)==FW_UPDATE_BUSY);
    CHECK(finish()==FW_UPDATE_NOT_READY);
    CHECK(chunk(32)==FW_UPDATE_BAD_CONFIG); CHECK(chunk(0)==FW_UPDATE_OK);
    CHECK(chunk(0)==FW_UPDATE_OK && programs==1U);
    CHECK(command(FW_UPDATE_MODE_APP,FW_UPDATE_ENTER,(const uint8_t *)"UPD3",4)==FW_UPDATE_OK);
    fw_update_cancel_action(); CHECK(fw_update_take_action()==FW_UPDATE_ACTION_NONE);
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_INFO,NULL,0)==FW_UPDATE_OK);
    CHECK(r32(reply+8+28)==0x12345678U && r32(reply+8+32)==32U);
    uint8_t word[40]; u32(word,0x12345678U); u32(word+4,0); memcpy(word+8,image,32); word[10]^=1;
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_WRITE,word,40)==FW_UPDATE_BAD_CONFIG);
    u32(word+4,UINT32_MAX); CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_WRITE,word,40)==FW_UPDATE_BAD_CONFIG);
    u32(word+4,IMAGE_BYTES); CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_WRITE,word,40)==FW_UPDATE_BAD_CONFIG);
    u32(word+4,1); CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_WRITE,word,40)==FW_UPDATE_BAD_CONFIG);
    u32(word,0); CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_WRITE,word,40)==FW_UPDATE_NOT_READY);
    for (uint32_t offset=32;offset<IMAGE_BYTES;offset+=32U) CHECK(chunk(offset)==FW_UPDATE_OK);
    CHECK(programs==IMAGE_BYTES/32U); CHECK(!fw_update_app_valid(NULL,NULL));
    CHECK(finish()==FW_UPDATE_OK && programs==IMAGE_BYTES/32U+1U);
    CHECK(finish()==FW_UPDATE_OK && programs==IMAGE_BYTES/32U+1U);
    uint32_t sp=0,pc=0; CHECK(fw_update_app_valid(&sp,&pc)); CHECK(sp==0x20020000U && pc==FW_UPDATE_APP_BASE+0x501U);
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_INFO,NULL,0)==FW_UPDATE_OK);
    CHECK(r32(reply+8+16)==IMAGE_BYTES && r32(reply+8+32)==IMAGE_BYTES && reply[14]==3);
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_RUN,(const uint8_t *)"UPD3",4)==FW_UPDATE_OK);
    CHECK(fw_update_take_action()==FW_UPDATE_ACTION_RUN && fw_update_take_action()==FW_UPDATE_ACTION_NONE);
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_ENTER,(const uint8_t *)"UPD3",4)==FW_UPDATE_OK);
    CHECK(fw_update_take_action()==FW_UPDATE_ACTION_NONE);
    uint8_t read_data[5]; u32(read_data,13); read_data[4]=48;
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_READ,read_data,5)==FW_UPDATE_OK && reply[7]==48);
    CHECK(!memcmp(reply+8,image+13,48));
    u32(read_data,FW_UPDATE_MAX_IMAGE-1U); read_data[4]=1;
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_READ,read_data,5)==FW_UPDATE_OK);
    u32(read_data,FW_UPDATE_MAX_IMAGE); CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_READ,read_data,5)==FW_UPDATE_BAD_CONFIG);
    u32(read_data,UINT32_MAX); CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_READ,read_data,5)==FW_UPDATE_BAD_CONFIG);
    u32(read_data,0); read_data[4]=0; CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_READ,read_data,5)==FW_UPDATE_BAD_CONFIG);
    read_data[4]=49; CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_READ,read_data,5)==FW_UPDATE_BAD_CONFIG);
    fw_update_init(); CHECK(fw_update_app_valid(NULL,NULL)); CHECK(finish()==FW_UPDATE_NOT_READY);
    canaries();
}
static void pre_erase_validation(void)
{
    static const unsigned bytes[]={0,11,12,13,14,15};
    for (unsigned i=0;i<sizeof bytes/sizeof bytes[0];i++) {
        reset_fixture(true); uint8_t m[32]; manifest(m); m[bytes[i]]^=1;
        CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_BEGIN,m,32)==FW_UPDATE_BAD_CONFIG);
        CHECK(!erases && !programs && fw_update_app_valid(NULL,NULL)); canaries();
    }
    static const uint32_t sizes[]={0,32,1024,1055,1057,FW_UPDATE_MAX_IMAGE+32U,UINT32_MAX};
    for (unsigned i=0;i<sizeof sizes/sizeof sizes[0];i++) {
        reset_fixture(true); uint8_t m[32]; manifest(m); u32(m+20,sizes[i]);
        CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_BEGIN,m,32)==FW_UPDATE_BAD_CONFIG); CHECK(!erases);
    }
    reset_fixture(true); uint8_t m[32]; manifest(m); u32(m+16,0);
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_BEGIN,m,32)==FW_UPDATE_BAD_CONFIG && !erases);
    manifest(m); u32(m+28,0);
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_BEGIN,m,32)==FW_UPDATE_BAD_CONFIG && !erases);
    manifest(m); physical_kib=512;
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_BEGIN,m,32)==FW_UPDATE_BAD_CONFIG && !erases);
    physical_kib=1024; flash_memory[FW_UPDATE_IDENTITY_OFFSET]=0;
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_BEGIN,m,32)==FW_UPDATE_BAD_CONFIG && !erases);
    reset_fixture(true); runtime_ready=false;
    CHECK(start()==FW_UPDATE_STORAGE_ERROR && !erases); CHECK(fw_update_app_valid(NULL,NULL));
    CHECK(!fw_update_flash_program(0,image)); runtime_ready=true;
    CHECK(!fw_update_flash_program(1,image)); CHECK(!fw_update_flash_program(0x40000U,image));
    CHECK(!fw_update_flash_program(UINT32_MAX,image)); CHECK(!fw_update_flash_program(0,NULL));
    canaries();
}
static void bad_images(void)
{
    for (unsigned kind=0;kind<12U;kind++) {
        reset_fixture(false);
        if (kind==0U) image[FW_UPDATE_IDENTITY_OFFSET+9]^=1;
        if (kind==1U) image[FW_UPDATE_IDENTITY_OFFSET+8]=77;
        if (kind==2U) image[FW_UPDATE_IDENTITY_OFFSET+12]^=1;
        if (kind==3U) image[FW_UPDATE_IDENTITY_OFFSET+16]^=1;
        if (kind==4U) image[FW_UPDATE_IDENTITY_OFFSET+28]^=1;
        if (kind==5U) u32(image,0x24080000U);
        if (kind==6U) u32(image,0x20000000U);
        if (kind==7U) u32(image,0x2001ffffU);
        if (kind==8U) u32(image+4,FW_UPDATE_APP_BASE+0x500U);
        if (kind==9U) u32(image+4,FW_UPDATE_BOOT_BASE+1U);
        if (kind==10U) u32(image+4,FW_UPDATE_APP_BASE+IMAGE_BYTES+1U);
        CHECK(start()==FW_UPDATE_OK);
        if (kind==11U) image[1536]^=1; /* expected CRC remains the original */
        transfer_image(); CHECK(finish()==FW_UPDATE_BAD_CONFIG);
        CHECK(programs==IMAGE_BYTES/32U && !fw_update_app_valid(NULL,NULL));
        CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_RUN,(const uint8_t *)"UPD3",4)==FW_UPDATE_NOT_READY);
        CHECK(fw_update_take_action()==FW_UPDATE_ACTION_NONE); canaries();
    }
}
static void fault_injection(void)
{
    static const unsigned cuts[]={0,32,SECTOR/2U,SECTOR};
    for (unsigned operation=1;operation<=2U;operation++) for (unsigned i=0;i<sizeof cuts/sizeof cuts[0];i++) {
        reset_fixture(true); fail_erase=operation; erase_cut=cuts[i];
        CHECK(start()==FW_UPDATE_STORAGE_ERROR); CHECK(start()==FW_UPDATE_STORAGE_ERROR);
        CHECK(erases==operation && !programs); fw_update_init();
        /* An interrupted first erase may leave the entire old valid image.
         * It is never acceptable to validate any changed old application. */
        if (fw_update_app_valid(NULL,NULL)) CHECK(!memcmp(flash_memory+SLOT_OFFSET,image,IMAGE_BYTES));
        if (operation==2U || cuts[i]==SECTOR) CHECK(!fw_update_app_valid(NULL,NULL));
        canaries();
    }
    static const unsigned failing_words[]={1,2,17,33,64};
    for (unsigned i=0;i<sizeof failing_words/sizeof failing_words[0];i++) for (unsigned cut=0;cut<=32U;cut++) {
        reset_fixture(true); CHECK(start()==FW_UPDATE_OK);
        fail_program=failing_words[i]; program_cut=cut;
        for (unsigned word=1;word<fail_program;word++) CHECK(chunk((word-1U)*32U)==FW_UPDATE_OK);
        uint32_t offset=(fail_program-1U)*32U;
        CHECK(chunk(offset)==FW_UPDATE_STORAGE_ERROR);
        CHECK(chunk(offset)==FW_UPDATE_STORAGE_ERROR && programs==fail_program);
        CHECK(finish()==FW_UPDATE_STORAGE_ERROR); fw_update_init(); CHECK(!fw_update_app_valid(NULL,NULL));
        fail_program=0; CHECK(start()==FW_UPDATE_OK); transfer_image(); CHECK(finish()==FW_UPDATE_OK);
        CHECK(fw_update_app_valid(NULL,NULL)); canaries();
    }
    for (unsigned cut=0;cut<=32U;cut++) {
        reset_fixture(true); CHECK(start()==FW_UPDATE_OK); transfer_image();
        fail_program=IMAGE_BYTES/32U+1U; program_cut=cut;
        CHECK(finish()==(cut==32U?FW_UPDATE_OK:FW_UPDATE_STORAGE_ERROR));
        CHECK(programs==IMAGE_BYTES/32U+1U);
        fw_update_init(); CHECK(fw_update_app_valid(NULL,NULL)==(cut==32U)); canaries();
    }
    reset_fixture(true); flash_memory[SLOT_OFFSET+1700]^=1;
    CHECK(!fw_update_app_valid(NULL,NULL)); fw_update_init();
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_INFO,NULL,0)==FW_UPDATE_OK && !(reply[14]&2U));
    reset_fixture(true); ecc_bad[(SLOT_OFFSET+32U)/32U]=1;
    CHECK(!fw_update_app_valid(NULL,NULL));
}
#else
static void disabled_writer(void)
{
    reset_fixture(true); uint8_t m[32]; manifest(m);
    CHECK(command(FW_UPDATE_MODE_BOOT,FW_UPDATE_BEGIN,m,32)==
          (FW_UPDATE_BOOT?FW_UPDATE_BAD_CONFIG:FW_UPDATE_NOT_READY));
    CHECK(!fw_update_flash_erase()); CHECK(!fw_update_flash_program(0,image));
    CHECK(!erases && !programs); canaries();
}
#endif
int main(void)
{
    common_tests();
#if FW_UPDATE_BOOT && HARDWARE_REVISION==86
    happy_and_bounds(); pre_erase_validation(); bad_images(); fault_injection();
#else
    disabled_writer();
#endif
    printf("fw_update C%d role%u boot%d: %u checks passed\n",HARDWARE_REVISION,ROLE,FW_UPDATE_BOOT,checks);
    return 0;
}
