/* Native 9288 launcher. The embedded resident remains outside 02700000. */
#include "dsys.h"
#include "resident_runtime.h"
#include "resident_loader.h"
#include "usb_cdc.h"
#include "usb_log.h"
#include "resident_blob.h"
typedef unsigned int u32;
typedef unsigned char u8;
#include "profiles.h"

#ifndef DEBUG_IMAGE_ID
#define DEBUG_IMAGE_ID 0u
#endif
static char message[420];
d88_usb_log_report usb_log_last_report;
int usb_log_last_result;
char usb_log_last_path[24];
unsigned usb_last_dialog_state;

static unsigned text(unsigned at,const char *s)
{
    while(*s && at+1u<sizeof(message)) message[at++]=*s++;
    message[at]=0;return at;
}
static unsigned hex(unsigned at,unsigned v)
{
    const char *digits="0123456789ABCDEF";
    unsigned i;
    at=text(at,"0x");
    for(i=0;i<8u && at+1u<sizeof(message);i++) message[at++]=digits[(v>>(28u-i*4u))&15u];
    message[at]=0;return at;
}
static void show(const char *body,const char *title)
{
    tpDL_GUITable->MessageBox(HWND_DESKTOP,(const T_BYTE*)body,(const T_BYTE*)title,MB_OK);
}
static int captured_profile(void)
{
    unsigned i,b;
    const probe_profile *profile=profiles+1;
    for(i=0;i<6u;i++) if(*(volatile unsigned*)(0x02000200u+i*4u)!=profile->tables[i]) return 0;
    for(i=0;i<3u;i++) for(b=0;b<40u;b++)
        if(*(volatile unsigned char*)(profile->signature_addresses[i]+b)!=profile->signatures[i][b]) return 0;
    return *(volatile unsigned*)0x02069000u==0x02500000u &&
           *(volatile unsigned*)0x02069004u==0x02700000u;
}
static int heap_range(unsigned address,unsigned size)
{
    return address>=0x02500000u && address<0x02700000u && size<=0x02700000u-address;
}
static int disk_idle(unsigned tcb)
{
    /* VBUS may remain high for power. Require the native disk exit path's
     * cold controller and idle flags instead of requiring a cable removal.
     * The worker repeats the full identity/ownership checks under CPU lock. */
    return *(volatile unsigned*)0x024ef004u==0u &&
           *(volatile unsigned*)0x02049798u==0u &&
           *(volatile unsigned*)0x020497e0u==0u &&
           *(volatile unsigned*)0x0204979cu==0x00040000u &&
           *(volatile unsigned char*)0x02000351u==0u &&
           *(volatile unsigned char*)(tcb+3u*72u+0x3fu)==7u &&
           (*(volatile unsigned char*)0x00300f20u&0x37u)==0x27u;
}
static void show_disk_active(void)
{
    show("Exit the 9288 USB disk screen before starting debug. The USB cable can stay connected for power.","D88DEBUG EXIT USB");
}
static void *allocate_resident(unsigned size)
{
    void *memory;
    if(tpDL_ROS33Table->ros33_dis_dsp()!=0) return 0;
    memory=tpDL_CRTLTable->CRTL_malloc(size);
    tpDL_ROS33Table->ros33_ena_dsp();
    return memory;
}
static void free_unstarted_resident(void *memory)
{
    if(tpDL_ROS33Table->ros33_dis_dsp()!=0) return;
    tpDL_CRTLTable->CRTL_free(memory);
    tpDL_ROS33Table->ros33_ena_dsp();
}
static volatile d88_control *existing_resident(unsigned worker)
{
    unsigned base;
    volatile d88_control *control;
    if(worker<0x02500000u+D88_RESIDENT_WORKER_OFFSET) return 0;
    base=worker-D88_RESIDENT_WORKER_OFFSET;
    if(!heap_range(base,D88_RESIDENT_MEMORY_SIZE)) return 0;
    control=(volatile d88_control*)(base+D88_RESIDENT_CONTROL_OFFSET);
    if(control->magic!=D88_CONTROL_MAGIC || control->version!=D88_CONTROL_VERSION ||
       control->structure_bytes!=sizeof(d88_control) || control->image_id!=DEBUG_IMAGE_ID ||
       control->self_base!=base || control->allocation_bytes!=D88_RESIDENT_MEMORY_SIZE ||
       control->task_id!=D88_TASK_ID || !control->task_created ||
       control->worker_address!=worker ||
       control->start_address!=base+D88_RESIDENT_START_OFFSET ||
       control->stop_address!=base+D88_RESIDENT_REQUEST_STOP_OFFSET) return 0;
    return control;
}
static void log_register(unsigned address,unsigned value)
{
    unsigned n=usb_log_last_report.register_count;
    if(n<sizeof(usb_log_last_report.registers)/sizeof(usb_log_last_report.registers[0])) {
        usb_log_last_report.registers[n].address=address;
        usb_log_last_report.registers[n].value=value;
        usb_log_last_report.register_count=n+1u;
    }
}
static int log_unlock(volatile d88_control *control)
{
    if(tpDL_ROS33Table->ros33_unl_cpu()==0) return 0;
    control->stop_requested=1;control->last_error=(unsigned)-304;control->state=D88_FAILED;
    return -304;
}
static unsigned char log_usb_read(volatile d88_control *control,unsigned address)
{
#ifdef USB_PLATFORM_TEST_NATIVE_GUEST
    /* The native integration replaces only the USB window. Use that same
     * guest backend for diagnostics; QEMU does not model the real FIFO core. */
    return ((unsigned char(*)(unsigned))(control->self_base+D88_USB_TEST_READ_OFFSET))(address);
#else
    (void)control;return *(volatile unsigned char*)address;
#endif
}
static int capture_usb_log(volatile d88_control *control)
{
    static const unsigned char offsets[]={
        0,1,2,3,4,7,8,9,10,11,16,17,18,19,20,21,
        0x20,0x21,0x22,0x23,0x24,0x2e,0x2f,
        0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
        0x38,0x39,0x3a,0x3b,0x3f,0x88,0x89,0x8a,0x8b,0x90,0x91,0x92
    };
    static const unsigned platform[]={0x00300f20u,0x00300f35u,0x00040273u,0x000402a6u,
        0x00040283u,0x000402a9u,0x00048132u,0x00048133u};
    static const unsigned globals[]={0x024ef004u,0x02049798u,0x020497e0u,0x0204979cu};
    static const unsigned flags[]={0x0200034cu,0x0200034du,0x02000350u,0x02000351u};
    volatile unsigned *source;
    unsigned tcb,task3,task8,i;
    int result,resume_result,unlock_result;
    usb_log_last_result=0;usb_log_last_path[0]=0;
    if(control->state!=D88_RUNNING) return 0;
    if(*(volatile unsigned char*)0x0206866cu || (*(volatile unsigned char*)0x0206866du&3u))
        return -301;
    if((unsigned)tpDL_ROS33Table->ros33_sus_tsk!=0x021cfe60u ||
       (unsigned)tpDL_ROS33Table->ros33_rsm_tsk!=0x021cfed2u ||
       (unsigned)tpDL_ROS33Table->ros33_loc_cpu!=0x021ce8aeu ||
       (unsigned)tpDL_ROS33Table->ros33_unl_cpu!=0x021ce8d8u) return -301;
    if(tpDL_ROS33Table->ros33_loc_cpu()!=0) return -301;
    tcb=*(volatile unsigned*)0x022a9820u;task3=tcb+3u*72u;task8=tcb+D88_TASK_ID*72u;
    if((tcb&3u) || tcb<0x02000000u || tcb>0x02500000u-9u*72u ||
       *(volatile unsigned*)0x022a9864u!=tcb || *(volatile unsigned*)0x022a9868u!=tcb ||
       *(volatile unsigned*)(task8+24u)!=control->worker_address ||
       !*(volatile unsigned char*)(task8+63u) ||
       (*(volatile unsigned char*)(task8+63u)&0x18u) ||
       *(volatile unsigned*)(task3+24u)!=0x0219c1f2u ||
       *(volatile unsigned*)(task3+20u)!=0x0202a394u ||
       *(volatile unsigned char*)(task3+61u)!=6u ||
       !(*(volatile unsigned char*)(task3+63u)&8u) ||
       *(volatile unsigned*)0x020000a8u!=control->self_base+D88_USB_PLATFORM_TIMER_GUARD_OFFSET ||
       (*(volatile unsigned char*)0x000402a6u&0x40u)) {
        result=log_unlock(control);return result?result:-302;
    }
    result=tpDL_ROS33Table->ros33_sus_tsk(D88_TASK_ID);
    if(result || !(*(volatile unsigned char*)(task8+63u)&8u)) {
        resume_result=0;
        if(*(volatile unsigned char*)(task8+63u)&8u)
            resume_result=tpDL_ROS33Table->ros33_rsm_tsk(D88_TASK_ID);
        unlock_result=log_unlock(control);
        if(resume_result || unlock_result) {
            control->stop_requested=1;control->last_error=(unsigned)-304;control->state=D88_FAILED;
            return -304;
        }
        return -303;
    }
    /* No native USB path can run, and the resident cannot process a host
     * STOP while the normal filesystem writes this foreground diagnostic. */
    for(i=0;i<sizeof(usb_log_last_report);i++) ((unsigned char*)&usb_log_last_report)[i]=0;
    usb_log_last_report.image_id=DEBUG_IMAGE_ID;usb_log_last_report.flags=5u;
    usb_log_last_report.capture_kind=1u;
    source=(volatile unsigned*)control;
    for(i=0;i<17u;i++) usb_log_last_report.control_words[i]=source[i];
    source=(volatile unsigned*)(control->self_base+D88_USB_DIAGNOSTIC_OFFSET);
    for(i=0;i<27u;i++) usb_log_last_report.diag_words[i]=source[i];
    for(i=0;i<sizeof(platform)/sizeof(platform[0]);i++)
        log_register(platform[i],*(volatile unsigned char*)platform[i]);
    if((*(volatile unsigned char*)0x00048133u&0x11u)==0x11u &&
       !(*(volatile unsigned char*)0x00048132u&0x11u) &&
       (*(volatile unsigned char*)0x00048124u&0x47u)==0x42u &&
       (*(volatile unsigned char*)0x0004812eu&1u) &&
       !(*(volatile unsigned char*)0x00300f35u&0x80u) &&
       (*(volatile unsigned char*)0x00300f20u&0x30u)==0x10u &&
       log_usb_read(control,0x01000020u)==0x12u) {
        usb_log_last_report.flags|=2u;
        for(i=0;i<sizeof(offsets);i++) log_register(0x01000000u+offsets[i],
            log_usb_read(control,0x01000000u+offsets[i]));
    }
    for(i=0;i<sizeof(globals)/sizeof(globals[0]);i++)
        log_register(globals[i],*(volatile unsigned*)globals[i]);
    for(i=0;i<sizeof(flags)/sizeof(flags[0]);i++)
        log_register(flags[i],*(volatile unsigned char*)flags[i]);
    log_register(0x022a9820u,tcb);
    log_register(task3+24u,*(volatile unsigned*)(task3+24u));
    log_register(task3+20u,*(volatile unsigned*)(task3+20u));
    log_register(task3+63u,*(volatile unsigned char*)(task3+63u));
    log_register(task8+24u,*(volatile unsigned*)(task8+24u));
    log_register(task8+20u,*(volatile unsigned*)(task8+20u));
    log_register(task8+63u,*(volatile unsigned char*)(task8+63u));
    for(i=0;i<18u;i++) log_register(control->self_base+D88_DEVICE_DESCRIPTOR_OFFSET+i,
        *(volatile unsigned char*)(control->self_base+D88_DEVICE_DESCRIPTOR_OFFSET+i));
    log_register(control->self_base+D88_USB_PLATFORM_MISC_FAILURE_OFFSET,
        *(volatile unsigned*)(control->self_base+D88_USB_PLATFORM_MISC_FAILURE_OFFSET));
    log_register(0x02068674u,*(volatile unsigned*)0x02068674u);
    if(log_unlock(control)!=0) return -304;
    usb_log_last_result=d88_usb_log_save(&usb_log_last_report,usb_log_last_path);
    control->stop_requested=1;
    if(*(volatile unsigned char*)0x0206866cu || (*(volatile unsigned char*)0x0206866du&3u)) {
        control->last_error=(unsigned)-304;control->state=D88_FAILED;return -304;
    }
    if(tpDL_ROS33Table->ros33_loc_cpu()!=0) {
        control->last_error=(unsigned)-304;control->state=D88_FAILED;return -304;
    }
    resume_result=tpDL_ROS33Table->ros33_rsm_tsk(D88_TASK_ID);
    if(log_unlock(control)!=0 || resume_result!=0) {
        control->last_error=(unsigned)-304;control->state=D88_FAILED;return -304;
    }
    return usb_log_last_result;
}
static void report(volatile d88_control *control,int result)
{
    unsigned n,i;
    volatile usb_cdc_diagnostic *diagnostic=(volatile usb_cdc_diagnostic*)
        (control->self_base+D88_USB_DIAGNOSTIC_OFFSET);
    volatile unsigned *misc_failure=(volatile unsigned*)
        (control->self_base+D88_USB_PLATFORM_MISC_FAILURE_OFFSET);
    for(i=0;i<1000u && (control->state==D88_STARTING || control->state==D88_STOPPING || control->state==D88_WAIT_USB ||
        (control->state==D88_RUNNING && control->stop_requested));i++) {
        /* This GUI task cannot wait for itself to dispatch a file-close job.
         * The retained entry executes one trusted GUI step outside ROS locks. */
        if(control->stop_requested || control->state==D88_STOPPING)
            (void)((int(*)(void))(control->self_base+D88_D88_FILES_GUI_PROGRESS_OFFSET))();
        tpDL_ROS33Table->ros33_dly_tsk(1);
    }
    usb_last_dialog_state=control->state;
    if(control->state==D88_RUNNING && !control->stop_requested) {
        n=text(0,"Background debug started.\nUSB=");n=hex(n,diagnostic->status);
        n=text(n," CTRL=");n=hex(n,diagnostic->control);
        n=text(n,"\nXCVR=");n=hex(n,diagnostic->xcvr);
        n=text(n," SETUP=");n=hex(n,diagnostic->setup_count);
        text(n,"\nFiles and GUI keys ready. Auto power-off paused.\nReturn to menu; use PC client. USB power can stay connected. Open again to stop.");
        show(message,"D88DEBUG READY");
    } else if(control->state==D88_WAIT_USB || control->state==D88_STOPPING ||
              (control->state==D88_RUNNING && control->stop_requested)) {
        usb_last_dialog_state=D88_WAIT_USB;
        show("Debug is stopping. USB restoration is still in progress. The cable can stay connected.","D88DEBUG USB WAIT");
    } else if(control->state==D88_STOPPED) {
        n=text(0,"Background debug stopped. Normal USB service restored.");
        if(usb_log_last_path[0]) {n=text(n,"\nUSB report: ");n=text(n,usb_log_last_path);}
        if(usb_log_last_result) {n=text(n,"\nLog error=");hex(n,(unsigned)usb_log_last_result);}
        show(message,"D88DEBUG STOPPED");
    } else {
        n=text(0,"State=");n=hex(n,control->state);
        n=text(n,"\nError=");n=hex(n,control->last_error?control->last_error:(unsigned)result);
        n=text(n,"\nPHASE=");n=hex(n,diagnostic->phase);
        n=text(n," MASK=");n=hex(n,diagnostic->failure_mask);
        n=text(n,"\nF20=");n=hex(n,diagnostic->misc_f20);
        n=text(n," F35=");n=hex(n,diagnostic->clock_f35);
        n=text(n,"\nUSBIRQ=");n=hex(n,diagnostic->usb_irq);
        n=text(n," TIMER=");n=hex(n,diagnostic->timer_irq);
        n=text(n,"\nBCU=");n=hex(n,(diagnostic->bcu_hi<<8)|diagnostic->bcu_lo);
        n=text(n,"\nMISCFAIL=");hex(n,*misc_failure);
        show(message,"D88DEBUG STATUS");
    }
}

T_WORD App_Main(void)
{
    unsigned tcb,slot,worker;
    int result;
    resident_loaded loaded;
    d88_startup configuration;
    volatile d88_control *control;
    if(!captured_profile()) {
        /* The basic SDK GUI prefix is the same as the verified probe. */
        show("This build requires the captured 9288 firmware. No debug state changed.","D88DEBUG PROFILE");
        return -1;
    }
    tcb=*(volatile unsigned*)0x022a9820u;
    if((tcb&3u) || tcb<0x02000000u || tcb>0x02500000u-9u*72u ||
       *(volatile unsigned*)0x022a9850u!=tcb) return -2;
    slot=tcb+D88_TASK_ID*72u;
    worker=*(volatile unsigned*)(slot+24u);
    if(worker) {
        control=existing_resident(worker);
        if(!control) {
            show("Task 8 is occupied by another build or application. Restart the device before replacing a resident.","D88DEBUG BUSY");
            return -3;
        }
        if(control->state==D88_RUNNING || control->state==D88_STARTING) {
            result=capture_usb_log(control);
            if(result==-304) {report(control,result);return result;}
            if(result) usb_log_last_result=result;
            ((void(*)(void))control->stop_address)();
            report(control,0);return 0;
        }
        if(control->state==D88_WAIT_USB || control->state==D88_STOPPING) {
            report(control,0);return 0;
        }
        if(!disk_idle(tcb)) {
            show_disk_active();return -4;
        }
        result=((int(*)(void))control->start_address)();
        report(control,result);return result;
    }
    if(*(volatile unsigned*)(slot+20u) || *(volatile unsigned char*)(slot+63u)) {
        show("Task 8 is not unused. Debug was not started.","D88DEBUG BUSY");return -5;
    }
    if(!disk_idle(tcb)) {
        show_disk_active();return -4;
    }
    result=resident_image_allocate(&d88_resident_blob,allocate_resident,free_unstarted_resident,&loaded);
    if(result!=0) {hex(text(0,"Resident load error "),(unsigned)result);show(message,"D88DEBUG ERROR");return result;}
    if(!heap_range((unsigned)loaded.memory,loaded.size)) {
        free_unstarted_resident(loaded.allocation);return -6;
    }
    configuration.self_base=(unsigned)loaded.memory;
    configuration.allocation_bytes=loaded.size;
    configuration.allocation_raw=(unsigned)loaded.allocation;
    configuration.ros=(unsigned)tpDL_ROS33Table;configuration.gui=(unsigned)tpDL_GUITable;
    configuration.fs=(unsigned)tpDL_FSTable;configuration.crtl=(unsigned)tpDL_CRTLTable;
    configuration.audio=(unsigned)tpDL_AudioTable;configuration.dict=(unsigned)tpDL_DictTable;
    control=(volatile d88_control*)((unsigned)loaded.memory+D88_RESIDENT_CONTROL_OFFSET);
    result=((int(*)(const d88_startup*))loaded.entry_address)(&configuration);
    if(!control->task_created) {
        hex(text(0,"Native task start error "),(unsigned)result);
        show(message,"D88DEBUG ERROR");
        free_unstarted_resident(loaded.allocation);return result;
    }
    /* Native TCB/saved context now references this allocation, even on error. */
    report(control,result);return result;
}
