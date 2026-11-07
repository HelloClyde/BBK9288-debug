/* Execute the production probe with a simulated SDK and address space.
 * This models errors and side effects; it is not a firmware or hardware test. */
#include <stdint.h>
#include <string.h>
#include "probe_stubs/dsys.h"

struct FS_FILE {
    unsigned char bytes[1024];
    unsigned length, exists, preexisting, writable;
};
static struct FS_FILE files[5];
static unsigned general[7], fault, specific_reads, signature_reads, mmio_reads, write_opens;
static unsigned profile_index;
static unsigned preexisting_write_attempts, dialog_count;
static char titles[16][80], messages[16][256];

static unsigned model_read32(unsigned);
static unsigned char model_read8(unsigned);
static unsigned model_pointer(uintptr_t);
#define PROBE_READ32(a) model_read32(a)
#define PROBE_READ8(a) model_read8(a)
#define PROBE_POINTER(p) model_pointer((uintptr_t)(p))
#include "../device/probe.c"

static int slot(const char *path)
{
    unsigned i;
    for(i=0;i<5;i++) if(!strcmp(path,names[i])) return (int)i;
    return -1;
}
static FS_FILE *model_open(const char *path,const char *mode)
{
    int i=slot(path);
    FS_FILE *f;
    if(i<0) return 0;
    f=&files[i];
    if(!strcmp(mode,"wb")) {
        write_opens++;
        if(f->preexisting) preexisting_write_attempts++;
        if(fault==1) return 0;
        f->exists=1; f->writable=1; f->length=0;
        return f;
    }
    if(strcmp(mode,"rb") || !f->exists) return 0;
    if(fault==4 && !f->preexisting) return 0;
    f->writable=0;
    return f;
}
static int model_close(FS_FILE *f)
{
    if(fault==3 && f->writable) return -1;
    if(fault==6 && !f->writable && !f->preexisting) return -1;
    return 0;
}
static size_t model_write(const void *input,size_t size,size_t count,FS_FILE *f)
{
    size_t n=size*count;
    if(fault==2 && n) n--;
    if(n>sizeof(f->bytes)) return 0;
    memcpy(f->bytes,input,n); f->length=(unsigned)n;
    return size ? n/size : 0;
}
static size_t model_read(void *output,size_t size,size_t count,FS_FILE *f)
{
    size_t n=size*count;
    if(n>f->length) n=f->length;
    if(fault==5 && n) n--;
    memcpy(output,f->bytes,n);
    if(fault==7 && n) ((unsigned char*)output)[0]^=1;
    return size ? n/size : 0;
}
static T_WORD model_dialog(int parent,const T_BYTE *body,const T_BYTE *title,unsigned style)
{
    (void)parent; (void)style;
    if(dialog_count<16) {
        strncpy(titles[dialog_count],(const char*)title,79);
        strncpy(messages[dialog_count],(const char*)body,255);
        dialog_count++;
    }
    return 0;
}
static T_GUI_RelocationTable gui={model_dialog};
static T_FS_RelocationTable fs={model_open,model_close,model_read,model_write};
T_GUI_RelocationTable *tpDL_GUITable=&gui;
T_FS_RelocationTable *tpDL_FSTable=&fs;

static unsigned model_pointer(uintptr_t p)
{
    static const unsigned fs_addresses[2][4]={
        {0x0214366a,0x02143744,0x02143936,0x02143a58},
        {0x021ab7ac,0x021ab886,0x021aba78,0x021abb9a}
    };
    if(p==(uintptr_t)&gui) return general[1];
    if(p==(uintptr_t)&fs) return general[2];
    if(p==(uintptr_t)model_dialog) return 0x02100100u;
    if(p==(uintptr_t)model_open) return fault==8 ? 1u : fs_addresses[profile_index][0];
    if(p==(uintptr_t)model_close) return fs_addresses[profile_index][1];
    if(p==(uintptr_t)model_read) return fs_addresses[profile_index][2];
    if(p==(uintptr_t)model_write) return fs_addresses[profile_index][3];
    return 0;
}
static unsigned model_read32(unsigned a)
{
    if(a>=0x02000200u && a<0x0200021cu && !(a&3u)) return general[(a-0x02000200u)/4u];
    specific_reads++;
    if(a<0x02000000u) mmio_reads++;
    if(a==profiles[profile_index].tcb_pointer) return profile_index ? 0x02310000u : 0x02260000u;
    return a^0x11223344u;
}
static unsigned char model_read8(unsigned a)
{
    unsigned i,offset;
    for(i=0;i<3;i++) {
        if(a>=profiles[profile_index].signature_addresses[i] &&
           a<profiles[profile_index].signature_addresses[i]+40u) {
            offset=a-profiles[profile_index].signature_addresses[i];
            signature_reads++;
            return profiles[profile_index].signatures[i][offset]^
                ((fault==9u+i && offset==30u) ? 1u : 0u);
        }
    }
    specific_reads++;
    if(a<0x02000000u) mmio_reads++;
    return (unsigned char)a;
}

void probe_model_reset(unsigned profile_id,unsigned error,unsigned existing)
{
    unsigned i;
    profile_index=profile_id==2u ? 1u : 0u;
    memcpy(general,profiles[profile_index].tables,sizeof(profiles[profile_index].tables));
    general[6]=0;
    if(!profile_id) {general[0]+=0x100;general[1]+=0x100;general[4]+=0x100;}
    fault=error; specific_reads=signature_reads=mmio_reads=write_opens=0;
    preexisting_write_attempts=dialog_count=0;
    memset(files,0,sizeof(files)); memset(titles,0,sizeof(titles)); memset(messages,0,sizeof(messages));
    for(i=0;i<5;i++) if(existing&(1u<<i)) {
        files[i].exists=files[i].preexisting=1;
        files[i].length=1024;
        memset(files[i].bytes,0xa0+(int)i,sizeof(files[i].bytes));
    }
}
int probe_model_run(void){return App_Main();}
unsigned probe_model_specific_reads(void){return specific_reads;}
unsigned probe_model_signature_reads(void){return signature_reads;}
unsigned probe_model_mmio_reads(void){return mmio_reads;}
unsigned probe_model_write_opens(void){return write_opens;}
unsigned probe_model_preexisting_write_attempts(void){return preexisting_write_attempts;}
unsigned probe_model_dialog_count(void){return dialog_count;}
const char *probe_model_title(unsigned i){return i<dialog_count ? titles[i] : "";}
const char *probe_model_message(unsigned i){return i<dialog_count ? messages[i] : "";}
unsigned probe_model_length(unsigned i){return i<5 && files[i].exists ? files[i].length : 0;}
const unsigned char *probe_model_file(unsigned i){return i<5 ? files[i].bytes : 0;}
