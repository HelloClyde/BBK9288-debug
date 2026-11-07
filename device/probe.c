/* Probe v3: persist runtime SDK inventory before profile-specific reads. */
#include "dsys.h"
typedef unsigned int u32;
typedef unsigned char u8;
#include "profiles.h"
static u32 report[256];
static u8 verify_buffer[1024];
static char dialog[240];
static const char *const names[]={"A:\\DBG88P01.BIN","A:\\DBG88P02.BIN","A:\\DBG88P03.BIN","A:\\DBG88P04.BIN","A:\\DBG88P05.BIN"};
#ifndef PROBE_READ32
#define PROBE_READ32(a) (*(volatile u32 *)(a))
#define PROBE_READ8(a) (*(volatile u8 *)(a))
#endif
#ifndef PROBE_POINTER
#define PROBE_POINTER(p) ((u32)(p))
#endif
static int ram(u32 a,u32 n){return a>=0x02000000u && a<0x02800000u && n<=0x02800000u-a;}
static int code(u32 a){return !(a&1u) && a>=0x02000000u && a<0x02500000u;}
static u32 word(u32 a){return PROBE_READ32(a);}
static void copy_words(unsigned at,u32 a,unsigned n){unsigned i;if((a&3u)||at>256u||n>256u-at||!ram(a,n*4u))return;for(i=0;i<n;i++)report[at+i]=word(a+i*4u);}
static unsigned append(unsigned at,const char *s){while(*s && at+1u<sizeof(dialog))dialog[at++]=*s++;dialog[at]=0;return at;}
static unsigned hex(unsigned at,u32 v){static const char d[]="0123456789ABCDEF";unsigned i;at=append(at,"0x");for(i=0;i<8u && at+1u<sizeof(dialog);i++)dialog[at++]=d[(v>>(28u-i*4u))&15u];dialog[at]=0;return at;}
static int gui_valid(void){return ram(PROBE_POINTER(tpDL_GUITable),__builtin_offsetof(T_GUI_RelocationTable,MessageBox)+4u) && code(PROBE_POINTER(tpDL_GUITable->MessageBox));}
static void show(const char *body,const char *title){if(gui_valid())tpDL_GUITable->MessageBox(HWND_DESKTOP,(const T_BYTE*)body,(const T_BYTE*)title,MB_OK);}
static int fs_valid(void){return ram(PROBE_POINTER(tpDL_FSTable),16u) && code(PROBE_POINTER(tpDL_FSTable->fopen)) && code(PROBE_POINTER(tpDL_FSTable->fclose)) && code(PROBE_POINTER(tpDL_FSTable->fread)) && code(PROBE_POINTER(tpDL_FSTable->fwrite));}
static void seal(void){unsigned i,b;u32 crc=0xffffffffu;const u8 *p=(const u8*)report;for(i=0;i<1020u;i++){crc^=p[i];for(b=0;b<8;b++)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}report[255]=crc^0xffffffffu;}
static int save(const char *path){unsigned n,i;int closed;FS_FILE*f;seal();f=tpDL_FSTable->fopen(path,"wb");if(!f)return -20;n=tpDL_FSTable->fwrite(report,1,sizeof(report),f);closed=tpDL_FSTable->fclose(f);if(n!=sizeof(report))return -21;if(closed!=0)return -22;f=tpDL_FSTable->fopen(path,"rb");if(!f)return -23;n=tpDL_FSTable->fread(verify_buffer,1,sizeof(verify_buffer),f);closed=tpDL_FSTable->fclose(f);if(n!=sizeof(verify_buffer)||closed!=0)return -24;for(i=0;i<sizeof(verify_buffer);i++)if(verify_buffer[i]!=((const u8*)report)[i])return -25;return 0;}
static void error(int result,const char *path){unsigned n=append(0,"Stage error ");n=hex(n,(u32)(-result));if(path){n=append(n,"\n");n=append(n,path);}n=append(n,"\nFS=");n=hex(n,word(0x02000208u));n=append(n,"\nGUI=");hex(n,word(0x02000204u));show(dialog,"D88PRB3 ERROR");}

static const probe_profile *match_profile(void)
{
    unsigned p,i,b,mask;
    for(p=0;p<PROBE_PROFILE_COUNT;p++) {
        for(i=0;i<6u;i++) if(report[4+i]!=profiles[p].tables[i]) break;
        if(i!=6u) continue;
        report[11]=profiles[p].id;
        mask=0;
        for(i=0;i<3u;i++) {
            for(b=0;b<40u;b++) {
                u8 observed=PROBE_READ8(profiles[p].signature_addresses[i]+b);
                if(b<16u) ((u8*)(report+208+i*4u))[b]=observed;
                if(observed!=profiles[p].signatures[i][b]) break;
            }
            if(b==40u) mask|=1u<<i;
        }
        report[29]=mask;
        if(mask==7u) return profiles+p;
        return 0;
    }
    return 0;
}

static void collect_known(const probe_profile *profile)
{
    unsigned i;
    u32 tcb;
    copy_words(16,profile->heap_state,8);
    report[24]=word(profile->tcb_pointer); tcb=report[24];
    report[25]=1; report[26]=8; report[27]=72;
    if(!(tcb&3u) && ram(tcb,9u*72u)) copy_words(32,tcb+72u,8u*18u);
    report[176]=word(0x00048120u); report[177]=word(0x00048124u);
    report[178]=word(0x00048128u); report[179]=word(0x0004812cu);
    report[180]=word(0x00048130u); report[181]=word(0x0004813cu);
    report[182]=PROBE_READ8(0x00300f20u); report[183]=PROBE_READ8(0x00300f35u);
    report[184]=PROBE_READ8(0x000402a6u); report[185]=PROBE_READ8(0x000402a9u);
    report[186]=PROBE_READ8(0x00300022u); report[187]=PROBE_READ8(0x00040273u);
    report[188]=PROBE_READ8(0x00040283u);
    report[192]=word(profile->usb_desktop); report[193]=word(profile->usb_phase);
    report[194]=word(profile->usb_phase2); report[195]=word(profile->usb_io_base);
    for(i=0;i<8;i++) report[196+i]=PROBE_READ8(profile->usb_flags+i);
    report[224]=profile->tcb_pointer; report[225]=profile->heap_state;
    report[226]=profile->usb_desktop; report[227]=profile->usb_phase;
    report[228]=profile->usb_phase2; report[229]=profile->usb_io_base;
    report[230]=profile->usb_flags;
    report[232]=word(0x020000a8u); report[233]=word(0x020001c4u);
    for(i=0;i<3u;i++) report[234+i]=profile->signature_addresses[i];
    report[238]=word(0x02000000u);
    report[241]=PROBE_READ8(0x00380200u); report[242]=PROBE_READ8(0x00380202u);
}
T_WORD App_Main(void)
{
    unsigned i,selected=5u,n;
    int known,result;
    FS_FILE *f;
    const probe_profile *profile;
    show("Probe v3 started. Press OK to save a report.","D88PRB3 START");
    for(i=0;i<256u;i++)report[i]=0;
    report[0]=0x50383844u;report[1]=3;report[2]=sizeof(report);
    copy_words(4,0x02000200u,7);
    profile=match_profile(); known=profile!=0;
    report[3]=known?1u:0u;report[28]=1u;
    if(!fs_valid()){error(-10,0);return -10;}
    report[12]=PROBE_POINTER(tpDL_FSTable->fopen);report[13]=PROBE_POINTER(tpDL_FSTable->fclose);
    report[14]=PROBE_POINTER(tpDL_FSTable->fread);report[15]=PROBE_POINTER(tpDL_FSTable->fwrite);
    for(i=0;i<5u;i++){f=tpDL_FSTable->fopen(names[i],"rb");if(f){tpDL_FSTable->fclose(f);continue;}selected=i;break;}
    if(selected==5u){show("DBG88P01..05 already exist; originals preserved.","D88PRB3 FILES EXIST");return -11;}
    /* A minimal report survives even if a later profile-specific read fails. */
    result=save(names[selected]);if(result){error(result,names[selected]);return result;}
    if(known){collect_known(profile);report[28]=2u;result=save(names[selected]);if(result){error(result,names[selected]);return result;}}
    n=append(0,known?"Full report saved and read back:\n":"Table-only report saved and read back:\n");
    n=append(n,names[selected]);
    if(!known){n=append(n,"\nROS=");n=hex(n,report[4]);n=append(n,"\nGUI=");n=hex(n,report[5]);n=append(n,"\nCRT=");hex(n,report[8]);}
    show(dialog,"D88PRB3 SAVED");return 0;
}
