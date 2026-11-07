/* Heap resident worker. The generated loader relocates every image reference;
 * the native OS tables are explicitly copied from the launch configuration. */
#include "dsys.h"
#include "resident_runtime.h"
#include "debug_core.h"
#include "usb_cdc.h"
#include "usb_platform.h"
#include "usb_delay.h"
#include "input_jobs.h"
#include "power_idle.h"
#include "file_runtime.h"
#include "task_compat_signatures.h"

#ifndef DEBUG_IMAGE_ID
#define DEBUG_IMAGE_ID 0u
#endif

T_GUI_RelocationTable *tpDL_GUITable;
T_CRTL_RelocationTable *tpDL_CRTLTable;
T_FS_RelocationTable *tpDL_FSTable;
T_ROS33_RelocationTable *tpDL_ROS33Table;
T_Audio_RelocationTable *tpDL_AudioTable;
T_Dict_RelocationTable *tpDL_DictTable;

volatile d88_control resident_control = {
    D88_CONTROL_MAGIC,D88_CONTROL_VERSION,sizeof(d88_control),DEBUG_IMAGE_ID,
    0,0,0,D88_TASK_ID,0,D88_NEW,0,0,0,0,0,0,0
};
static dbg_state core;
static unsigned char snapshot[320u*240u];
static unsigned char worker_stack[D88_STACK_BYTES] __attribute__((aligned(16)));
static unsigned char input[64];
static unsigned input_count,input_offset,tx_offset,transport_session;

static void clear_bytes(void *memory,unsigned size)
{
    unsigned char *p=(unsigned char*)memory;
    while(size--) *p++=0;
}
static int in_ram(unsigned address,unsigned size)
{
    return address>=0x02000000u && address<0x02800000u && size<=0x02800000u-address;
}
static dbg_u32 uptime(void *unused)
{
    (void)unused; return 0; /* Native tick-to-ms conversion remains unverified. */
}
static int read_ram(void *unused,dbg_u32 address,unsigned char *out,dbg_u32 size)
{
    unsigned i;
    (void)unused;
    if(!in_ram(address,size)) return -(int)DBG_INVALID;
    for(i=0;i<size;i++) out[i]=*(volatile unsigned char*)(address+i);
    return 0;
}
static int queue_key(void *unused,dbg_u32 scan)
{
    (void)unused;
    if(!scan || scan>127u) return -(int)DBG_INVALID;
    if(d88_input_pending()) return -(int)DBG_BUSY;
    return d88_input_queue(scan)==0?0:-(int)DBG_IO;
}
static int capture(void *unused,unsigned char *out,dbg_u32 capacity,dbg_screen *screen)
{
    unsigned mode=*(volatile unsigned char*)0x00380200u;
    unsigned bpp=1u<<(mode&7u),x,y,i,value,offset,shift;
    unsigned char palette[256];
    (void)unused;
    /* Full 320x240 at 8bpp exceeds the chip's 40KiB internal VRAM. */
    if((mode&7u)>2u || capacity<320u*240u) return -(int)DBG_INVALID;
    if((*(volatile unsigned short*)0x00380042u+1u)*8u!=320u ||
       *(volatile unsigned short*)0x0038004cu+1u!=240u ||
       *(volatile unsigned short*)0x00380210u!=0u) return -(int)DBG_INVALID;
    for(i=0;i<(1u<<bpp);i++) {
        if(mode&0x10u)
            palette[i]=(unsigned char)(bpp<8u ? i*(bpp==1u?255u:(bpp==2u?85u:17u)) : ((i>>2)<<2)|(i>>6));
        else {
            value=*(volatile unsigned char*)(0x00380401u+i*4u);
            palette[i]=(unsigned char)((value&0xfcu)|(value>>6));
        }
    }
    for(y=0;y<240u;y++) for(x=0;x<320u;x++) {
        offset=y*(320u*bpp/8u)+x*bpp/8u;
        shift=8u-bpp-((x*bpp)&7u);
        out[y*320u+x]=palette[(*(volatile unsigned char*)(0x003c0000u+offset)>>shift)&((1u<<bpp)-1u)];
    }
    screen->width=320; screen->height=240; screen->bpp=8; screen->stride=320;
    screen->format=1; screen->flags=0;
    return 320*240;
}

static void configure_core(void)
{
    dbg_platform platform;
    clear_bytes(&platform,sizeof(platform));
    platform.image_id=DEBUG_IMAGE_ID;
    platform.caps=DBG_CAP_READ|DBG_CAP_WRITE|DBG_CAP_SCREEN|DBG_CAP_STOP|DBG_CAP_SCRATCH;
    platform.flags=DBG_FLAG_HARDWARE_UNVERIFIED|DBG_FLAG_UPTIME_UNAVAILABLE|DBG_FLAG_GUI_KEYS_ONLY;
    platform.scratch_address=(dbg_u32)core.scratch;
    platform.snapshot=snapshot; platform.snapshot_capacity=sizeof(snapshot);
    platform.uptime_ms=uptime; platform.read=read_ram; platform.capture=capture;
    d88_input_reset();
    if(d88_input_available()) {platform.caps|=DBG_CAP_KEY;platform.key=queue_key;}
    if(files_diagnostic.enabled) {
        platform.caps|=DBG_CAP_FILES;
        platform.file_rpc=d88_files_rpc;platform.file_reset=d88_files_reset;
    }
    dbg_init(&core,&platform);
    input_count=input_offset=tx_offset=0;
    transport_session=usb_get_session();
}

static void service_poll(void)
{
    unsigned size,used;
    int count;
    const unsigned char *reply;
    usb_poll();
    if(usb_diagnostic.phase==USB_CDC_DIAG_POLL && (int)usb_diagnostic.result<0) {
        resident_control.last_error=usb_diagnostic.result;
        resident_control.stop_requested=1;
        return;
    }
    if(transport_session!=usb_get_session()) {
        transport_session=usb_get_session(); dbg_reset_session(&core);
        input_count=input_offset=tx_offset=0; d88_input_cancel();
    }
    d88_input_poll();
    reply=dbg_reply(&core,&size);
    if(reply) {
        if(tx_offset<size) {
            count=usb_send(reply+tx_offset,(int)(size-tx_offset));
            if(count>0) tx_offset+=(unsigned)count;
        } else if(usb_tx_idle()) {
            dbg_consume_reply(&core); tx_offset=0;
        }
    } else if(usb_get_dtr()) {
        if(input_offset==input_count) {
            count=usb_receive(input,sizeof(input));
            input_count=count>0?(unsigned)count:0u; input_offset=0;
        }
        if(input_count>input_offset) {
            used=dbg_feed(&core,input+input_offset,input_count-input_offset);
            input_offset+=used;
        }
    }
    if(core.stop_requested) resident_control.stop_requested=1;
    resident_control.polls++;
}

void resident_request_stop(void)
{
    resident_control.stop_requested=1;
}

void resident_worker(void)
{
    int result,cleanup;
    unsigned budget;
    resident_control.state=D88_STARTING;
    result=d88_power_idle_begin();
    if(result==0) result=usb_platform_claim();
    if(result==0) result=usb_start();
    if(result==0) result=d88_usb_detach_delay();
    if(result==0) result=usb_platform_check();
    if(result==0 && !resident_control.stop_requested)
        result=usb_attach(D88_USB_DETACH_MS);
    if(result==0 && !resident_control.stop_requested) result=d88_files_start();
    if(result==0 && !resident_control.stop_requested) {
        configure_core();
        resident_control.starts++;
        resident_control.state=D88_RUNNING;
        while(!resident_control.stop_requested) {
            result=d88_power_idle_poll();
            if(result!=0) {resident_control.last_error=(d88_word)result;break;}
            result=usb_platform_check();
            if(result!=0) {resident_control.last_error=(d88_word)result;break;}
            result=d88_files_poll();
            if(result!=0) {resident_control.last_error=(d88_word)result;break;}
            for(budget=0;budget<8u && !resident_control.stop_requested;budget++) service_poll();
            tpDL_ROS33Table->ros33_dly_tsk(1);
        }
    } else resident_control.last_error=(d88_word)result;
    if(result==0 && (int)resident_control.last_error<0)
        result=(int)resident_control.last_error;
    resident_control.state=D88_STOPPING;
    /* Keep native USB ownership until GUI has closed/verified or cancelled all
     * file handles. Session reset/STOP only signal cancellation in the worker. */
    do {
        cleanup=d88_files_shutdown_poll();
        if(cleanup<0) {
            resident_control.last_error=(d88_word)cleanup;
            resident_control.state=D88_FAILED;d88_power_idle_end();
            for(;;) tpDL_ROS33Table->ros33_tslp_tsk(TMO_FEVR);
        }
        if(cleanup) {
            (void)d88_power_idle_poll();usb_poll();
            tpDL_ROS33Table->ros33_dly_tsk(1);
        }
    } while(cleanup);
    /* No preference was changed; after cleanup the normal idle countdown
     * resumes from the last refresh, even when the cable still supplies power. */
    d88_power_idle_end();
    d88_input_cancel();
    while(d88_input_pending()) {
        if(d88_input_poll()==-3) {
            resident_control.last_error=(d88_word)-106;
            result=-106;break;
        }
        tpDL_ROS33Table->ros33_dly_tsk(1);
    }
    /* The bus ACK proves transfer completion, not that the host application
     * has consumed the reply. Leave a short, bounded OS scheduling window. */
    for(budget=0;budget<8u;budget++) {usb_poll();tpDL_ROS33Table->ros33_dly_tsk(1);}
    usb_stop();
    do {
        cleanup=usb_platform_release();
        if(cleanup==1) {
            resident_control.state=D88_WAIT_USB;
            cleanup=d88_usb_detach_delay();
            if(cleanup==0) cleanup=usb_platform_finish_release(D88_USB_DETACH_MS);
        }
        if(cleanup<0) {
            /* Preserve the allocation and stopped native ownership for
             * diagnosis. Never blindly replay a failed restore operation. */
            resident_control.last_error=(d88_word)cleanup;
            resident_control.state=D88_FAILED;
            for(;;) tpDL_ROS33Table->ros33_tslp_tsk(TMO_FEVR);
        }
        if(cleanup!=0) tpDL_ROS33Table->ros33_dly_tsk(1);
    } while(cleanup!=0);
    resident_control.state=result==0?D88_STOPPED:D88_FAILED;
    tpDL_ROS33Table->ros33_ext_tsk();
    for(;;) tpDL_ROS33Table->ros33_dly_tsk(1000);
}

int resident_start(void)
{
    unsigned tcb_base,slot,stack_top,old_current,i;
    int result;
    if(resident_control.state==D88_RUNNING || resident_control.state==D88_STARTING ||
       resident_control.state==D88_STOPPING || resident_control.state==D88_WAIT_USB) return -101;
    tcb_base=*(volatile unsigned*)0x022a9820u;
    if((tcb_base&3u) || !in_ram(tcb_base,9u*72u) ||
       *(volatile unsigned*)0x022a9850u!=tcb_base) return -102;
    if((unsigned)tpDL_ROS33Table->ros33_sta_tsk!=0x021cf9b2u ||
       (unsigned)tpDL_ROS33Table->ros33_vcre_tsk!=0x021ced06u ||
       (unsigned)tpDL_ROS33Table->ros33_loc_cpu!=0x021ce8aeu ||
       (unsigned)tpDL_ROS33Table->ros33_unl_cpu!=0x021ce8d8u) return -107;
    for(i=0;i<sizeof(sta_entry_signature);i++)
        if(*(volatile unsigned char*)(0x021cf9b2u+i)!=sta_entry_signature[i]) return -107;
    for(i=0;i<sizeof(sta_current_signature);i++)
        if(*(volatile unsigned char*)(0x021cfa52u+i)!=sta_current_signature[i]) return -107;
    /* loc/unl are not nested locks. Enter only from ordinary task context. */
    if(*(volatile unsigned char*)0x0206866cu ||
       (*(volatile unsigned char*)0x0206866du&3u)) return -108;
    slot=tcb_base+D88_TASK_ID*72u;
    stack_top=((unsigned)(worker_stack+sizeof(worker_stack)))&~15u;
    if(tpDL_ROS33Table->ros33_loc_cpu()!=0) return -103;
    old_current=*(volatile unsigned*)0x02068664u;
    for(i=1;i<=8u;i++) if(old_current==tcb_base+i*72u) break;
    if(i>8u) {tpDL_ROS33Table->ros33_unl_cpu();return -109;}
    if(!resident_control.task_created) {
        if(*(volatile unsigned*)(slot+24u) || *(volatile unsigned*)(slot+20u) ||
           *(volatile unsigned char*)(slot+63u)) {
            tpDL_ROS33Table->ros33_unl_cpu(); return -104;
        }
        result=tpDL_ROS33Table->ros33_vcre_tsk(D88_TASK_ID,(FP)resident_worker,D88_TASK_PRIORITY,stack_top);
        if(result!=0) {tpDL_ROS33Table->ros33_unl_cpu();return result;}
        resident_control.task_created=1;
    } else if(*(volatile unsigned*)(slot+24u)!=(unsigned)resident_worker ||
              *(volatile unsigned*)(slot+20u)!=stack_top ||
              !(*(volatile unsigned char*)(slot+63u)&0x10u)) {
        tpDL_ROS33Table->ros33_unl_cpu();return -105;
    }
    resident_control.last_error=0; resident_control.stop_requested=0;
    resident_control.state=D88_STARTING;
    result=tpDL_ROS33Table->ros33_sta_tsk(D88_TASK_ID,0);
    /* This captured STA stores its target into the scheduler's current TCB.
     * Restore the actual caller before allowing IRQ or dispatch again. The
     * full-frame bug and this correction are covered by native ROS testing. */
    *(volatile unsigned*)0x02068664u=old_current;
    tpDL_ROS33Table->ros33_unl_cpu();
    if(result!=0) {resident_control.state=D88_FAILED;resident_control.last_error=(d88_word)result;}
    return result;
}

int resident_entry(const d88_startup *configuration)
{
    if(!configuration || configuration->self_base<0x02500000u ||
       configuration->self_base>=0x02700000u ||
       configuration->allocation_bytes>0x02700000u-configuration->self_base) return -100;
    tpDL_ROS33Table=(T_ROS33_RelocationTable*)configuration->ros;
    tpDL_GUITable=(T_GUI_RelocationTable*)configuration->gui;
    tpDL_FSTable=(T_FS_RelocationTable*)configuration->fs;
    tpDL_CRTLTable=(T_CRTL_RelocationTable*)configuration->crtl;
    tpDL_AudioTable=(T_Audio_RelocationTable*)configuration->audio;
    tpDL_DictTable=(T_Dict_RelocationTable*)configuration->dict;
    resident_control.self_base=configuration->self_base;
    resident_control.allocation_bytes=configuration->allocation_bytes;
    resident_control.allocation_raw=configuration->allocation_raw;
    resident_control.worker_address=(d88_word)resident_worker;
    resident_control.start_address=(d88_word)resident_start;
    resident_control.stop_address=(d88_word)resident_request_stop;
    return resident_start();
}
