/* Native filesystem persistence only; capture/USB serialization is caller-owned. */
#include "dsys.h"
#include "usb_log.h"

typedef char d88_log_word_is_32[(sizeof(d88_usb_log_word)==4u)?1:-1];
typedef char d88_log_size_is_1024[(sizeof(d88_usb_log_report)==1024u)?1:-1];
typedef char d88_log_control_at_32[(__builtin_offsetof(d88_usb_log_report,control_words)==32u)?1:-1];
typedef char d88_log_diag_at_128[(__builtin_offsetof(d88_usb_log_report,diag_words)==128u)?1:-1];
typedef char d88_log_registers_at_256[(__builtin_offsetof(d88_usb_log_report,registers)==256u)?1:-1];
typedef char d88_log_reserved_at_960[(__builtin_offsetof(d88_usb_log_report,reserved_words)==960u)?1:-1];
typedef char d88_log_crc_at_1020[(__builtin_offsetof(d88_usb_log_report,crc32)==1020u)?1:-1];

static d88_usb_log_report saved_report;
static unsigned char verify_buffer[D88_USB_LOG_BYTES];
#ifndef USB_LOG_POINTER
#define USB_LOG_POINTER(p) ((unsigned)(p))
#endif

static int ram(unsigned address,unsigned size)
{
    return address>=0x02000000u && address<0x02800000u && size<=0x02800000u-address;
}
static int code(unsigned address)
{
    return !(address&1u) && address>=0x02000000u && address<0x02500000u;
}
static int fs_valid(void)
{
    return ram(USB_LOG_POINTER(tpDL_FSTable),16u) &&
           code(USB_LOG_POINTER(tpDL_FSTable->fopen)) &&
           code(USB_LOG_POINTER(tpDL_FSTable->fclose)) &&
           code(USB_LOG_POINTER(tpDL_FSTable->fread)) &&
           code(USB_LOG_POINTER(tpDL_FSTable->fwrite));
}
static void seal(void)
{
    unsigned i,b,crc=0xffffffffu;
    const unsigned char *bytes=(const unsigned char*)&saved_report;
    for(i=0;i<1020u;i++) {
        crc^=bytes[i];
        for(b=0;b<8u;b++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    saved_report.crc32=crc^0xffffffffu;
}
static void filename(char path[24],unsigned index)
{
    static const char pattern[]="A:\\D88U00.BIN";
    unsigned i;
    for(i=0;i<sizeof(pattern);i++) path[i]=pattern[i];
    path[7]=(char)('0'+index/10u);path[8]=(char)('0'+index%10u);
}
int d88_usb_log_save(const d88_usb_log_report *report,char out_path[24])
{
    unsigned i,index,count;
    int closed;
    FS_FILE *file;
    char candidate[24];
    if(!out_path) return D88_USB_LOG_E_ARGUMENT;
    if(!report || report->register_count>D88_USB_LOG_REGISTERS) {
        out_path[0]=0;return D88_USB_LOG_E_ARGUMENT;
    }
    /* Snapshot before any file API or caller output mutation. */
    for(i=0;i<sizeof(saved_report);i++)
        ((unsigned char*)&saved_report)[i]=((const unsigned char*)report)[i];
    for(i=0;i<24u;i++) out_path[i]=0;
    saved_report.magic=D88_USB_LOG_MAGIC;saved_report.version=D88_USB_LOG_VERSION;
    saved_report.bytes=D88_USB_LOG_BYTES;seal();
    if(!fs_valid()) return D88_USB_LOG_E_FS;
    for(index=1u;index<=99u;index++) {
        filename(candidate,index);
        file=tpDL_FSTable->fopen(candidate,"rb");
        if(!file) break;
        if(tpDL_FSTable->fclose(file)!=0) return D88_USB_LOG_E_EXISTING_CLOSE;
    }
    if(index>99u) return D88_USB_LOG_E_FILES_EXIST;
    for(i=0;candidate[i];i++) out_path[i]=candidate[i];
    out_path[i]=0;
    file=tpDL_FSTable->fopen(out_path,"wb");
    if(!file) return D88_USB_LOG_E_CREATE;
    count=tpDL_FSTable->fwrite(&saved_report,1,sizeof(saved_report),file);
    closed=tpDL_FSTable->fclose(file);
    if(count!=sizeof(saved_report)) return D88_USB_LOG_E_WRITE;
    if(closed!=0) return D88_USB_LOG_E_CLOSE;
    file=tpDL_FSTable->fopen(out_path,"rb");
    if(!file) return D88_USB_LOG_E_READ_OPEN;
    count=tpDL_FSTable->fread(verify_buffer,1,sizeof(verify_buffer),file);
    closed=tpDL_FSTable->fclose(file);
    if(count!=sizeof(verify_buffer) || closed!=0) return D88_USB_LOG_E_READ;
    for(i=0;i<sizeof(verify_buffer);i++)
        if(verify_buffer[i]!=((const unsigned char*)&saved_report)[i]) return D88_USB_LOG_E_COMPARE;
    return 0;
}
