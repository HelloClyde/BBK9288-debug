/* Host model for the launcher's production native-FS persistence routine. */
#include <stdint.h>
#include <string.h>
#include "probe_stubs/dsys.h"
static unsigned model_pointer(uintptr_t pointer);
#define USB_LOG_POINTER(p) model_pointer((uintptr_t)(p))
#include "../device/usb_log.c"

struct FS_FILE {
    unsigned char data[1024];
    unsigned length,exists,preexisting,writable;
};
static FS_FILE files[99];
static unsigned fault,write_opens,old_write_attempts,close_calls,mutate_input;
static d88_usb_log_report input_report;

static int slot(const char *path)
{
    unsigned index;
    if(strlen(path)!=13u || memcmp(path,"A:\\D88U",7u) || strcmp(path+9u,".BIN") ||
       path[7]<'0' || path[7]>'9' || path[8]<'0' || path[8]>'9') return -1;
    index=(unsigned)(path[7]-'0')*10u+(unsigned)(path[8]-'0');
    return index>=1u && index<=99u ? (int)index-1 : -1;
}
static FS_FILE *model_open(const char *path,const char *mode)
{
    int index=slot(path);
    FS_FILE *file;
    if(index<0) return 0;
    file=&files[index];
    if(!strcmp(mode,"wb")) {
        write_opens++;
        if(file->preexisting) old_write_attempts++;
        if(fault==1u) return 0;
        if(mutate_input) input_report.diag_words[3]=0xdeadbeefu;
        file->exists=file->writable=1;file->length=0;return file;
    }
    if(strcmp(mode,"rb") || !file->exists) return 0;
    if(fault==4u && !file->preexisting) return 0;
    file->writable=0;return file;
}
static int model_close(FS_FILE *file)
{
    close_calls++;
    if(fault==3u && file->writable) return -1;
    if(fault==6u && !file->writable && !file->preexisting) return -1;
    if(fault==8u && file->preexisting) return -1;
    return 0;
}
static size_t model_write(const void *bytes,size_t size,size_t count,FS_FILE *file)
{
    size_t total=size*count;
    if(fault==2u && total) total--;
    if(total>sizeof(file->data)) return 0;
    memcpy(file->data,bytes,total);file->length=(unsigned)total;
    return size ? total/size : 0;
}
static size_t model_read(void *bytes,size_t size,size_t count,FS_FILE *file)
{
    size_t total=size*count;
    if(total>file->length) total=file->length;
    if(fault==5u && total) total--;
    memcpy(bytes,file->data,total);
    if(fault==7u && total) ((unsigned char*)bytes)[512]^=1u;
    return size ? total/size : 0;
}
static T_FS_RelocationTable fs={model_open,model_close,model_read,model_write};
T_FS_RelocationTable *tpDL_FSTable=&fs;
static unsigned model_pointer(uintptr_t pointer)
{
    if(pointer==(uintptr_t)&fs) return fault==9u ? 1u : 0x021e7ae0u;
    if(pointer==(uintptr_t)model_open) return fault==10u ? 1u : 0x021ab7acu;
    if(pointer==(uintptr_t)model_close) return 0x021ab886u;
    if(pointer==(uintptr_t)model_read) return 0x021aba78u;
    if(pointer==(uintptr_t)model_write) return 0x021abb9au;
    return 0;
}
void model_reset(unsigned error)
{
    unsigned i;
    memset(files,0,sizeof(files));memset(&input_report,0,sizeof(input_report));
    fault=error;write_opens=old_write_attempts=close_calls=mutate_input=0;
    input_report.image_id=0x12345678u;input_report.flags=3u;
    input_report.register_count=2u;input_report.capture_kind=7u;
    for(i=0;i<17u;i++) input_report.control_words[i]=0x100u+i;
    for(i=0;i<27u;i++) input_report.diag_words[i]=0x200u+i;
    input_report.registers[0].address=0x300f20u;input_report.registers[0].value=0x17u;
    input_report.registers[1].address=0x1000022u;input_report.registers[1].value=0xc0u;
}
void model_preexist(unsigned index)
{
    if(index>=99u) return;
    files[index].exists=files[index].preexisting=1u;
    files[index].length=1024u;memset(files[index].data,(int)(index+1u),1024u);
}
int model_save(char *path) {return d88_usb_log_save(&input_report,path);}
int model_null_report(char *path) {return d88_usb_log_save(0,path);}
int model_null_path(void) {return d88_usb_log_save(&input_report,0);}
void model_reg_count(unsigned count) {input_report.register_count=count;}
void model_mutate_input_on_open(unsigned enabled) {mutate_input=enabled;}
unsigned model_input_word(unsigned index)
{return index<256u ? ((unsigned*)&input_report)[index] : 0u;}
const unsigned char *model_data(unsigned index) {return index<99u ? files[index].data : 0;}
unsigned model_length(unsigned index) {return index<99u ? files[index].length : 0u;}
unsigned model_write_opens(void) {return write_opens;}
unsigned model_old_write_attempts(void) {return old_write_attempts;}
unsigned model_close_calls(void) {return close_calls;}
