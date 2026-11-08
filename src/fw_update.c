#include "fw_update.h"
#include "fw_update_storage.h"
#include <string.h>

#if ROLE_LEFT
#define TARGET_ROLE 0U
#else
#define TARGET_ROLE 1U
#endif

typedef struct {
    uint32_t session;
    fw_update_image image;
    uint32_t next;
    uint8_t manifest[32];
    bool failed, committed;
} update_session;
static update_session transfer;
static fw_update_image checked_image;
static bool checked_valid;
static unsigned pending_action;

static uint16_t get16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t *p, uint16_t n)
{ p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }
static void put32(uint8_t *p, uint32_t n)
{ for (unsigned i=0;i<4U;i++) p[i]=(uint8_t)(n>>(8U*i)); }

uint16_t fw_update_crc16(const uint8_t *bytes, size_t length)
{
    uint16_t crc=0xffffU;
    for (size_t i=0;i<length;i++) {
        crc^=(uint16_t)((uint16_t)bytes[i]<<8);
        for (unsigned bit=0;bit<8U;bit++)
            crc=(uint16_t)((crc<<1)^((crc&0x8000U)?0x1021U:0U));
    }
    return crc;
}
static uint32_t crc32_more(uint32_t crc, const uint8_t *bytes, size_t length)
{
    for (size_t i=0;i<length;i++) {
        crc^=bytes[i];
        for (unsigned bit=0;bit<8U;bit++)
            crc=(crc>>1)^((crc&1U)?0xedb88320UL:0U);
    }
    return crc;
}
uint32_t fw_update_crc32(const uint8_t *bytes, size_t length)
{ return ~crc32_more(UINT32_MAX,bytes,length); }

static bool image_size_valid(uint32_t size)
{ return size>=FW_UPDATE_MIN_IMAGE && size<=FW_UPDATE_MAX_IMAGE && (size&31U)==0U; }
static bool identity_valid(const uint8_t identity[32], bool boot, uint32_t version)
{
#if HARDWARE_REVISION==86
    const char *magic=boot?"C86BL003":"C86FW003";
    return !memcmp(identity,magic,8) &&
        identity[8]==86U && identity[9]==TARGET_ROLE && get16(identity+10)==1U &&
        get32(identity+12)==version && get32(identity+16)==FW_UPDATE_APP_BASE &&
        get32(identity+20)==FW_UPDATE_MAX_IMAGE &&
        get32(identity+24)==FW_UPDATE_IDENTITY_MAGIC &&
        get32(identity+28)==(uint32_t)~FW_UPDATE_IDENTITY_MAGIC;
#else
    (void)identity; (void)boot; (void)version;
    return false;
#endif
}
bool fw_update_boot_present(void)
{
    uint8_t identity[32];
    return fw_update_flash_kib()==1024U &&
        fw_update_flash_boot_identity(identity) &&
        identity_valid(identity,true,FW_UPDATE_BOOT_VERSION);
}

static uint32_t target_metadata_word(void)
{ return 86U | (TARGET_ROLE<<8) | (1U<<16); }
static void metadata_encode(uint8_t record[32], const fw_update_image *image)
{
    put32(record,FW_UPDATE_METADATA_MAGIC);
    put32(record+4,image->size); put32(record+8,image->crc32);
    put32(record+12,image->version); put32(record+16,target_metadata_word());
    put32(record+20,FW_UPDATE_APP_BASE);
    put32(record+24,fw_update_crc32(record,24));
    put32(record+28,(uint32_t)~FW_UPDATE_METADATA_MAGIC);
}
static bool metadata_read(fw_update_image *image)
{
    uint8_t record[32];
    if (HARDWARE_REVISION!=86 || fw_update_flash_kib()!=1024U ||
        !fw_update_flash_read(FW_UPDATE_MAX_IMAGE,record,sizeof record) ||
        get32(record)!=FW_UPDATE_METADATA_MAGIC ||
        get32(record+28)!=(uint32_t)~FW_UPDATE_METADATA_MAGIC ||
        get32(record+16)!=target_metadata_word() ||
        get32(record+20)!=FW_UPDATE_APP_BASE ||
        get32(record+24)!=fw_update_crc32(record,24) ||
        !image_size_valid(get32(record+4)) || !get32(record+12)) return false;
    image->size=get32(record+4); image->crc32=get32(record+8);
    image->version=get32(record+12);
    return true;
}
static bool image_valid(const fw_update_image *image, uint32_t *stack, uint32_t *reset)
{
    uint8_t word[32], data[256];
    if (!image_size_valid(image->size) || !image->version ||
        !fw_update_flash_read(0,word,32)) return false;
    uint32_t sp=get32(word),pc=get32(word+4);
    if (sp<=0x20000000UL || sp>0x20020000UL || (sp&7U)!=0U ||
        !(pc&1U) || pc<FW_UPDATE_APP_BASE+1U ||
        pc>=FW_UPDATE_APP_BASE+image->size) return false;
    if (!fw_update_flash_read(FW_UPDATE_IDENTITY_OFFSET,word,32) ||
        !identity_valid(word,false,image->version)) return false;
    uint32_t crc=UINT32_MAX;
    for (uint32_t offset=0;offset<image->size;) {
        uint32_t length=image->size-offset;
        if (length>sizeof data) length=sizeof data;
        if (!fw_update_flash_read(offset,data,length)) return false;
        crc=crc32_more(crc,data,length); offset+=length;
    }
    if (~crc!=image->crc32) return false;
    if (stack) *stack=sp;
    if (reset) *reset=pc;
    return true;
}
bool fw_update_app_valid(uint32_t *stack, uint32_t *reset)
{
    fw_update_image image;
    return metadata_read(&image) && image_valid(&image,stack,reset);
}
static void refresh_checked_image(void)
{
    memset(&checked_image,0,sizeof checked_image);
    checked_valid=metadata_read(&checked_image) && image_valid(&checked_image,NULL,NULL);
    if (!checked_valid) memset(&checked_image,0,sizeof checked_image);
}
void fw_update_init(void)
{
    memset(&transfer,0,sizeof transfer); pending_action=0;
    refresh_checked_image();
}
unsigned fw_update_take_action(void)
{ unsigned result=pending_action; pending_action=0; return result; }
void fw_update_cancel_action(void) { pending_action=0; }

static uint8_t begin(const uint8_t *payload)
{
    uint8_t uid[12]; fw_update_flash_uid(uid);
    uint32_t session=get32(payload+16);
    fw_update_image image={get32(payload+20),get32(payload+24),get32(payload+28)};
    if (HARDWARE_REVISION!=86 || fw_update_flash_kib()!=1024U ||
        memcmp(uid,payload,12) || payload[12]!=86U || payload[13]!=TARGET_ROLE ||
        get16(payload+14) || !session || !image_size_valid(image.size) ||
        !image.version || !fw_update_boot_present()) return FW_UPDATE_BAD_CONFIG;
    if (transfer.session) {
        if (session!=transfer.session || memcmp(transfer.manifest,payload,32)) return FW_UPDATE_BUSY;
        return transfer.failed?FW_UPDATE_STORAGE_ERROR:FW_UPDATE_OK;
    }
    transfer.session=session; transfer.image=image;
    memcpy(transfer.manifest,payload,32);
    /* Invalidate the checked cache before the first destructive action. The
     * backend erases the metadata-containing sector FIRST. A reset can then
     * never bless a partially replaced application using an old commit. */
    checked_valid=false; memset(&checked_image,0,sizeof checked_image);
    if (!fw_update_flash_erase()) { transfer.failed=true; return FW_UPDATE_STORAGE_ERROR; }
    return FW_UPDATE_OK;
}
static uint8_t write_word(const uint8_t *payload)
{
    uint32_t session=get32(payload),offset=get32(payload+4);
    if (!transfer.session || session!=transfer.session) return FW_UPDATE_NOT_READY;
    if (transfer.failed) return FW_UPDATE_STORAGE_ERROR;
    if ((offset&31U)!=0U || offset>transfer.image.size ||
        32U>transfer.image.size-offset) return FW_UPDATE_BAD_CONFIG;
    if (offset!=transfer.next) {
        /* Only the last acknowledged chunk may be retried, and it must be
         * byte-for-byte identical. Never program an ECC flashword twice. */
        uint8_t actual[32];
        if (transfer.next>=32U && offset==transfer.next-32U &&
            fw_update_flash_read(offset,actual,32) && !memcmp(actual,payload+8,32)) return FW_UPDATE_OK;
        return FW_UPDATE_BAD_CONFIG;
    }
    if (transfer.committed) return FW_UPDATE_BAD_CONFIG;
    if (!fw_update_flash_program(offset,payload+8)) {
        transfer.failed=true; return FW_UPDATE_STORAGE_ERROR;
    }
    transfer.next+=32U;
    return FW_UPDATE_OK;
}
static uint8_t commit(const uint8_t *payload)
{
    if (!transfer.session || get32(payload)!=transfer.session) return FW_UPDATE_NOT_READY;
    if (transfer.failed) return FW_UPDATE_STORAGE_ERROR;
    if (transfer.next!=transfer.image.size) return FW_UPDATE_NOT_READY;
    if (!image_valid(&transfer.image,NULL,NULL)) return FW_UPDATE_BAD_CONFIG;
    uint8_t record[32],actual[32]; metadata_encode(record,&transfer.image);
    if (transfer.committed) {
        return fw_update_flash_read(FW_UPDATE_MAX_IMAGE,actual,32) && !memcmp(record,actual,32)
            ? FW_UPDATE_OK : FW_UPDATE_STORAGE_ERROR;
    }
    if (!fw_update_flash_program(FW_UPDATE_MAX_IMAGE,record)) {
        /* A flash controller error or lost completion can coexist with a
         * fully valid final record. Read it and the whole image back; never
         * attempt to program this ECC flashword a second time. */
        if (!fw_update_flash_read(FW_UPDATE_MAX_IMAGE,actual,32) ||
            memcmp(record,actual,32) || !fw_update_app_valid(NULL,NULL)) {
            transfer.failed=true; return FW_UPDATE_STORAGE_ERROR;
        }
    }
    refresh_checked_image();
    if (!checked_valid) { transfer.failed=true; return FW_UPDATE_STORAGE_ERROR; }
    transfer.committed=true;
    return FW_UPDATE_OK;
}

bool fw_update_handle(const uint8_t request[64], uint8_t reply[64], uint8_t mode)
{
    if (!request || !reply || request[0]!='C' || request[1]!='7' ||
        request[3]<FW_UPDATE_INFO || request[3]>FW_UPDATE_READ) return false;
    memset(reply,0,64); reply[0]='C'; reply[1]='7'; reply[2]=1;
    reply[3]=request[3]|0x80U; reply[4]=request[4]; reply[5]=request[5];
    uint8_t status=FW_UPDATE_OK,length=0,*out=reply+8;
    const uint8_t *in=request+8;
    static const uint8_t sizes[]={0,4,32,40,4,4,5};
    if (request[2]!=1 || !get16(request+4) || request[6] || request[7]>54U ||
        fw_update_crc16(request,62)!=get16(request+62)) { status=FW_UPDATE_BAD_PACKET; goto done; }
    for (unsigned i=8U+request[7];i<62U;i++)
        if (request[i]) { status=FW_UPDATE_BAD_PACKET; goto done; }
    if (request[7]!=sizes[request[3]-FW_UPDATE_INFO]) { status=FW_UPDATE_BAD_PACKET; goto done; }
    if (mode>FW_UPDATE_MODE_APP || (!FW_UPDATE_BOOT && mode==FW_UPDATE_MODE_BOOT)) {
        status=FW_UPDATE_NOT_READY; goto done;
    }
    if (mode==FW_UPDATE_MODE_APP && request[3]!=FW_UPDATE_INFO &&
        request[3]!=FW_UPDATE_ENTER && request[3]!=FW_UPDATE_RUN) {
        status=FW_UPDATE_NOT_READY; goto done;
    }
    switch (request[3]) {
    case FW_UPDATE_INFO:
        out[0]=FW_UPDATE_PROTOCOL; out[1]=HARDWARE_REVISION; out[2]=TARGET_ROLE; out[3]=mode;
        put16(out+4,fw_update_flash_kib());
        put16(out+6,(uint16_t)((fw_update_boot_present()?1U:0U)|(checked_valid?2U:0U)));
        put32(out+8,FW_UPDATE_APP_BASE); put32(out+12,FW_UPDATE_MAX_IMAGE);
        put32(out+16,checked_image.size); put32(out+20,checked_image.crc32);
        put32(out+24,checked_image.version); put32(out+28,transfer.session);
        put32(out+32,transfer.next); fw_update_flash_uid(out+36); length=48; break;
    case FW_UPDATE_ENTER:
        if (memcmp(in,"UPD3",4)) { status=FW_UPDATE_BAD_CONFIG; break; }
        if (!fw_update_boot_present()) { status=FW_UPDATE_NOT_READY; break; }
        if (mode==FW_UPDATE_MODE_APP) pending_action=FW_UPDATE_ACTION_ENTER;
        break;
    case FW_UPDATE_BEGIN: status=begin(in); break;
    case FW_UPDATE_WRITE: status=write_word(in); break;
    case FW_UPDATE_COMMIT: status=commit(in); break;
    case FW_UPDATE_RUN:
        if (memcmp(in,"UPD3",4)) { status=FW_UPDATE_BAD_CONFIG; break; }
        if (mode==FW_UPDATE_MODE_BOOT) {
            if (!fw_update_app_valid(NULL,NULL)) { status=FW_UPDATE_NOT_READY; break; }
            pending_action=FW_UPDATE_ACTION_RUN;
        }
        break;
    case FW_UPDATE_READ: {
        uint32_t offset=get32(in); uint8_t count=in[4];
        if (!count || count>48U || offset>FW_UPDATE_MAX_IMAGE || count>FW_UPDATE_MAX_IMAGE-offset) {
            status=FW_UPDATE_BAD_CONFIG; break;
        }
        if (!fw_update_flash_read(offset,out,count)) { status=FW_UPDATE_STORAGE_ERROR; break; }
        length=count; break;
    }
    default: status=FW_UPDATE_BAD_COMMAND; break;
    }
done:
    reply[6]=status; reply[7]=status?0U:length;
    if (status) memset(out,0,54);
    put16(reply+62,fw_update_crc16(reply,62));
    return true;
}
