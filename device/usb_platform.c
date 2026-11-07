/* Captured 9288 kernel, entry0215EBC4. Never claims a live MSC session.
 * Native USB runs both as task3 and from Timer3B. Only its observed cold-idle
 * snoozing, clock-gated state is supported. USB snooze is not a controller
 * reset and does not imply that SRAM or configuration registers were cleared.
 * See docs/usb-platform-takeover.md for evidence and physical-test boundaries.
 */
#include "usb_platform.h"
#include "usb_delay.h"

#if D88_USB_DETACH_MS < USB_PLATFORM_CLOCK_POWERUP_MS
#error Native delay lower bound is too short for USB clock powerup
#endif

#if defined(USB_PLATFORM_TEST) && !defined(USB_PLATFORM_TEST_NATIVE_GUEST)
extern int usb_platform_test_powerup_delay(void);
#define POWERUP_DELAY() usb_platform_test_powerup_delay()
#else
#define POWERUP_DELAY() d88_usb_detach_delay()
#endif
typedef unsigned int u32;
typedef unsigned char u8;
#include "profiles.h"

#ifdef USB_PLATFORM_TEST
extern u8 usb_platform_test_read8(u32 address);
extern u32 usb_platform_test_read32(u32 address);
extern void usb_platform_test_write8(u32 address,u8 value);
extern void usb_platform_test_write32(u32 address,u32 value);
extern int usb_platform_test_syscall(unsigned operation,int argument);
#define R8(a) usb_platform_test_read8(a)
#define R32(a) usb_platform_test_read32(a)
#define W8(a,v) usb_platform_test_write8((a),(u8)(v))
#define W32(a,v) usb_platform_test_write32((a),(u32)(v))
#ifdef USB_PLATFORM_TEST_NATIVE_GUEST
extern void usb_platform_timer_guard(void);
#define TIMER_GUARD_ADDRESS ((u32)usb_platform_timer_guard)
#else
#define TIMER_GUARD_ADDRESS 0x02500100u
#endif
#else
#define R8(a) (*(volatile u8 *)(a))
#define R32(a) (*(volatile u32 *)(a))
#define W8(a,v) (*(volatile u8 *)(a)=(u8)(v))
#define W32(a,v) (*(volatile u32 *)(a)=(u32)(v))
extern void usb_platform_timer_guard(void);
#define TIMER_GUARD_ADDRESS ((u32)usb_platform_timer_guard)
#endif

enum { SYS_LOCK=1,SYS_UNLOCK=2,SYS_SUSPEND=3,SYS_RESUME=4 };
enum { USB=0x01000000u,MISC=0x00300f20u,TIMER_ENABLE=0x00040273u,
       USB_ENABLE=0x000402a6u,TIMER_FLAG=0x00040283u,USB_FLAG=0x000402a9u,IRQ_BIT=0x40u,
       TIMER_VECTOR=0x020000a8u,MISC_WRPROT=0x00300f2fu };

typedef struct usb_platform_context {
    int state,last_error;
    u32 tcb,original_timer_vector;
    u8 original_misc,timer_enable,usb_enable;
    u8 suspended,irqs_masked,controller_changed,sram_saved,guard_installed,detached;
    u8 clock_changed,lock_fault;
    u8 sram[1024];
} usb_platform_context;
static usb_platform_context owner;
volatile unsigned usb_platform_misc_failure;

static int ram(u32 address,u32 bytes)
{ return !(address&3u) && address>=0x02000000u && address<0x02800000u && bytes<=0x02800000u-address; }

static int system_call(unsigned operation,int argument)
{
#ifdef USB_PLATFORM_TEST
    return usb_platform_test_syscall(operation,argument);
#else
    u32 address;
    if(operation==SYS_LOCK) address=0x021ce8aeu;
    else if(operation==SYS_UNLOCK) address=0x021ce8d8u;
    else if(operation==SYS_SUSPEND) address=0x021cfe60u;
    else address=0x021cfed2u;
    if(operation==SYS_SUSPEND || operation==SYS_RESUME)
        return ((int (*)(int))address)(argument);
    return ((int (*)(void))address)();
#endif
}

static int profile_valid(void)
{
    const probe_profile *profile=&profiles[1];
    unsigned i,b;
    for(i=0;i<6u;i++) if(R32(0x02000200u+i*4u)!=profile->tables[i]) return 0;
    for(i=0;i<3u;i++) for(b=0;b<40u;b++)
        if(R8(profile->signature_addresses[i]+b)!=profile->signatures[i][b]) return 0;
    /* The audited resident linker/loader binds the guard to this heap image. */
    if(R32(TIMER_VECTOR)!=0x0219c9acu) return 0;
    if(R32(0x021e81e8u+9u*4u)!=0x021cfe60u ||
       R32(0x021e81e8u+10u*4u)!=0x021cfed2u ||
       R32(0x021e81e8u+23u*4u)!=0x021ce8aeu ||
       R32(0x021e81e8u+24u*4u)!=0x021ce8d8u) return 0;
    owner.tcb=R32(0x022a9820u);
    if(!ram(owner.tcb,9u*72u)) return 0;
    /* The ROS functions use separately initialized aliases of the same base. */
    if(R32(0x022a9864u)!=owner.tcb || R32(0x022a9868u)!=owner.tcb) return 0;
    return 1;
}

static int bus_valid(void)
{
    /* Existing native BCU mapping is sufficient; no BCU/CPU clock changes. */
    return (R8(0x00048133u)&0x11u)==0x11u && !(R8(0x00048132u)&0x11u) &&
           (R8(0x00048124u)&0x47u)==0x42u && (R8(0x0004812eu)&1u) &&
           !(R8(0x00300f35u)&0x80u);
}

static int native_idle(void)
{
    return R32(0x024ef004u)==0 && R32(0x02049798u)==0 &&
           R32(0x020497e0u)==0 && R32(0x0204979cu)==0x00040000u &&
           R8(0x02000351u)==0;
}

static int task_identity(void)
{
    u32 task=owner.tcb+3u*72u;
    return R32(0x022a9820u)==owner.tcb && R32(task+0x18u)==0x0219c1f2u &&
           R32(task+0x14u)==0x0202a394u && R8(task+0x3du)==6u;
}

static int connected(void) { return (R8(USB+0x22u)&0x80u)!=0; }
static void delay_bus_sync(void)
{
    volatile unsigned i;
    /* Snooze transitions synchronize over five bus clocks. Native firmware
     * waits16 loop iterations; retain a longer64-iteration bounded wait. */
    for(i=0;i<64u;i++) { }
}
static int write_usb_mode(u8 mode,int *clock_rose)
{
    u8 before,expected,actual;
    int result=0;
    /* Each protected write has its own short lock. Never call this helper
     * from an already locked claim/restore section. A GUI Misc write cannot
     * consume the single-use key between this unlock and its target write. */
    if(clock_rose) *clock_rose=0;
    if(system_call(SYS_LOCK,0)!=0) {owner.lock_fault=1;return USB_PLATFORM_E_LOCK;}
    before=R8(MISC); expected=(before&~0x30u)|(mode&0x30u);
    actual=before;
    /* A clock loss between wake stages must not turn the clock on and leave
     * snooze in one write, bypassing its powerup interval. */
    if(!(before&0x10u) && (expected&0x10u) && !(expected&0x20u)) result=USB_PLATFORM_E_BUS;
    else {W8(MISC_WRPROT,0x96u); W8(MISC,expected); actual=R8(MISC);}
    if(clock_rose && !(before&0x10u) && (actual&0x10u)) *clock_rose=1;
    if((before^actual)&0x30u) owner.clock_changed=1;
    if((actual&0x37u)!=(expected&0x37u)) {
        if(!usb_platform_misc_failure)
            usb_platform_misc_failure=((unsigned)expected<<8)|actual;
        result=USB_PLATFORM_E_BUS;
    }
    if(system_call(SYS_UNLOCK,0)!=0) {owner.lock_fault=1;result=USB_PLATFORM_E_LOCK;}
    return result;
}
static int clocked_snooze(void)
{
    int rose,result=write_usb_mode(0x30u,&rose);
    if(result) return result;
    if(rose) {
        /* Manual VI-2-24: T1+250ms<T2. Keep USBSNZ asserted throughout the
         * real clock-powerup delay; a later D+ disconnect delay is separate.
         * write_usb_mode has already released its short CPU lock here. */
        result=POWERUP_DELAY();
        if(result) return result;
        if((R8(MISC)&0x30u)!=0x30u || !bus_valid()) return USB_PLATFORM_E_BUS;
    }
    return 0;
}
static int wake_usb(void)
{
    int result=clocked_snooze();
    if(result) return result;
    delay_bus_sync();
    result=write_usb_mode(0x10u,0); /* leave snooze, keep USB clock enabled */
    if(result) return result;
    delay_bus_sync();
    return 0;
}
static int quiesce_usb(void)
{
    /* Fail closed before controller writes when silicon/bus access is no
     * longer known valid, including cleanup after a failed driver poll. */
    if(!bus_valid() || (R8(MISC)&0x30u)!=0x10u || R8(USB+0x20u)!=0x12u)
        return USB_PLATFORM_E_BUS;
    W8(USB+0x21u,0); W8(USB+0x23u,1);
    if((R8(USB+0x21u)&1u) || (R8(USB+0x23u)&0x80u)) return USB_PLATFORM_E_BUS;
    return 0;
}
static int native_cold_snooze(void)
{
    int result=clocked_snooze();
    if(result) return result;
    delay_bus_sync();
    result=write_usb_mode(owner.original_misc&0x30u,0);
    if(result) return result;
    /* write_usb_mode verifies clock/SNZ and preserved readable wait bits. */
    delay_bus_sync(); return 0;
}
static void descriptor_zero(void)
{ W8(USB+0x88u,0); W8(USB+0x89u,0); }
static void save_sram(void)
{
    unsigned i;
    descriptor_zero(); (void)R8(USB+0x8fu); /* documented dummy read */
    for(i=0;i<sizeof(owner.sram);i++) owner.sram[i]=R8(USB+0x8fu);
}
static int sram_matches(void)
{
    unsigned i;
    descriptor_zero(); (void)R8(USB+0x8fu);
    for(i=0;i<sizeof(owner.sram);i++) if(R8(USB+0x8fu)!=owner.sram[i]) return 0;
    return 1;
}
static void restore_sram(void)
{
    unsigned i;
    descriptor_zero();
    for(i=0;i<sizeof(owner.sram);i++) W8(USB+0x8fu,owner.sram[i]);
}

/* CPU is locked here. Keep original native paths masked if resume fails. */
static int resume_native_locked(void)
{
    int result;
    if(owner.guard_installed) {
        W8(TIMER_ENABLE,R8(TIMER_ENABLE)&~IRQ_BIT);
        if(R32(TIMER_VECTOR)!=TIMER_GUARD_ADDRESS) return USB_PLATFORM_E_RESTORE;
        W32(TIMER_VECTOR,owner.original_timer_vector);
        if(R32(TIMER_VECTOR)!=owner.original_timer_vector) return USB_PLATFORM_E_RESTORE;
        owner.guard_installed=0;
    }
    if(owner.suspended) {
        if(!task_identity() || !(R8(owner.tcb+3u*72u+0x3fu)&8u)) result=USB_PLATFORM_E_RESTORE;
        else result=system_call(SYS_RESUME,3);
        if(result!=0 || (R8(owner.tcb+3u*72u+0x3fu)&8u)) {
            /* Keep the resident guard if a failed resume leaves native paths
             * paused and the GUI later re-enables Timer3B. */
            if(owner.irqs_masked && R32(TIMER_VECTOR)==owner.original_timer_vector) {
                W32(TIMER_VECTOR,TIMER_GUARD_ADDRESS);owner.guard_installed=1;
            }
            return USB_PLATFORM_E_RESTORE;
        }
        owner.suspended=0;
    }
    if(owner.irqs_masked) {
        W8(TIMER_ENABLE,(R8(TIMER_ENABLE)&~IRQ_BIT)|owner.timer_enable);
        W8(USB_ENABLE,(R8(USB_ENABLE)&~IRQ_BIT)|owner.usb_enable);
        owner.irqs_masked=0;
    }
    return 0;
}
static int abandon_locked(int error)
{
    int result=resume_native_locked();
    if(system_call(SYS_UNLOCK,0)!=0) result=USB_PLATFORM_E_LOCK;
    owner.state=result?USB_PLATFORM_FAULT:USB_PLATFORM_IDLE;
    owner.last_error=result?result:error;
    return owner.last_error;
}
static int fail_after_change(int error)
{
    int result;
    owner.last_error=error;
    if(owner.lock_fault) {owner.state=USB_PLATFORM_FAULT;return error;}
    result=usb_platform_release();
    if(result<0) return result;
    owner.last_error=error;
    return error;
}

int usb_platform_claim(void)
{
    int result;
    if(owner.state!=USB_PLATFORM_IDLE) return USB_PLATFORM_E_BUSY;
    usb_platform_misc_failure=0;
    owner.last_error=0; owner.sram_saved=0; owner.controller_changed=0; owner.detached=0;
    owner.clock_changed=owner.lock_fault=0;
    if(!profile_valid()) return owner.last_error=USB_PLATFORM_E_PROFILE;
    if(!bus_valid()) return owner.last_error=USB_PLATFORM_E_BUS;
    if((R8(MISC)&0x37u)!=0x27u || !native_idle() || !task_identity() ||
       R8(owner.tcb+3u*72u+0x3fu)!=7u)
        return owner.last_error=USB_PLATFORM_E_BUSY;
    if(system_call(SYS_LOCK,0)!=0) return owner.last_error=USB_PLATFORM_E_LOCK;
    if(!bus_valid() || (R8(MISC)&0x37u)!=0x27u || !native_idle() || !task_identity() ||
       R8(owner.tcb+3u*72u+0x3fu)!=7u ||
       R32(TIMER_VECTOR)!=0x0219c9acu)
        return abandon_locked(USB_PLATFORM_E_BUSY);
    owner.state=USB_PLATFORM_CLAIMING;
    owner.original_misc=R8(MISC);
    owner.original_timer_vector=R32(TIMER_VECTOR);
    owner.timer_enable=R8(TIMER_ENABLE)&IRQ_BIT; owner.usb_enable=R8(USB_ENABLE)&IRQ_BIT;
    result=system_call(SYS_SUSPEND,3);
    owner.suspended=(R8(owner.tcb+3u*72u+0x3fu)&8u)!=0;
    if(result!=0 || !owner.suspended) return abandon_locked(USB_PLATFORM_E_TASK);
    W8(TIMER_ENABLE,R8(TIMER_ENABLE)&~IRQ_BIT);
    W8(USB_ENABLE,R8(USB_ENABLE)&~IRQ_BIT); owner.irqs_masked=1;
    W32(TIMER_VECTOR,TIMER_GUARD_ADDRESS);
    owner.guard_installed=R32(TIMER_VECTOR)!=owner.original_timer_vector;
    if(R32(TIMER_VECTOR)!=TIMER_GUARD_ADDRESS) return abandon_locked(USB_PLATFORM_E_LOST);
    owner.guard_installed=1;
    if(!bus_valid() || (R8(MISC)&0x37u)!=0x27u || !native_idle())
        return abandon_locked(USB_PLATFORM_E_BUSY);
    if(system_call(SYS_UNLOCK,0)!=0) {
        owner.state=USB_PLATFORM_FAULT; return owner.last_error=USB_PLATFORM_E_LOCK;
    }
    result=wake_usb();
    if(result) return fail_after_change(result);
    owner.controller_changed=1;
    result=quiesce_usb();
    if(result) return fail_after_change(result);
    if(R8(USB+0x20u)!=0x12u) return fail_after_change(USB_PLATFORM_E_BUS);
    save_sram();
    if(!sram_matches()) return fail_after_change(USB_PLATFORM_E_SRAM);
    owner.sram_saved=1;
    owner.state=USB_PLATFORM_CLAIMED; return 0;
}

int usb_platform_check(void)
{
    if(owner.state!=USB_PLATFORM_CLAIMED) return USB_PLATFORM_E_BUSY;
    if(!bus_valid() || (R8(MISC)&0x30u)!=0x10u ||
       R32(TIMER_VECTOR)!=TIMER_GUARD_ADDRESS || (R8(USB_ENABLE)&IRQ_BIT) ||
       !task_identity() || !(R8(owner.tcb+3u*72u+0x3fu)&8u) || !native_idle())
        return owner.last_error=USB_PLATFORM_E_LOST;
    return 0;
}

static int restore_native(void)
{
    int result;
    if(!bus_valid()) {
        owner.state=USB_PLATFORM_FAULT; return owner.last_error=USB_PLATFORM_E_LOST;
    }
    if(owner.guard_installed && R32(TIMER_VECTOR)!=TIMER_GUARD_ADDRESS) {
        owner.state=USB_PLATFORM_FAULT; return owner.last_error=USB_PLATFORM_E_LOST;
    }
    if(!native_idle() || !task_identity()) {
        owner.state=USB_PLATFORM_FAULT; return owner.last_error=USB_PLATFORM_E_LOST;
    }
    owner.state=USB_PLATFORM_RESTORING;
    if(owner.controller_changed) {
        result=wake_usb();
        if(!result) result=quiesce_usb();
        if(result) {owner.state=USB_PLATFORM_FAULT;return owner.last_error=result;}
        if(owner.sram_saved) {
            restore_sram();
            if(!sram_matches()) {
                owner.state=USB_PLATFORM_FAULT; return owner.last_error=USB_PLATFORM_E_RESTORE;
            }
        }
        /* Snooze retains controller state; explicitly verify ActiveUSB and
         * pullup are off before removing the clock. It is not a reset. */
        result=quiesce_usb();
        if(result) {owner.state=USB_PLATFORM_FAULT;return owner.last_error=result;}
    }
    if(owner.clock_changed) {
        result=native_cold_snooze();
        if(result) {owner.state=USB_PLATFORM_FAULT;return owner.last_error=result;}
        owner.clock_changed=0;
    }
    owner.controller_changed=0;
    if(system_call(SYS_LOCK,0)!=0) {
        owner.state=USB_PLATFORM_FAULT; return owner.last_error=USB_PLATFORM_E_LOCK;
    }
    /* Drop only paused USB-poll timer events; retain the keyboard timer flag. */
    W8(TIMER_FLAG,IRQ_BIT);
    W8(USB_FLAG,IRQ_BIT);
    result=resume_native_locked();
    if(system_call(SYS_UNLOCK,0)!=0) result=USB_PLATFORM_E_LOCK;
    if(result) { owner.state=USB_PLATFORM_FAULT; return owner.last_error=result; }
    owner.state=USB_PLATFORM_IDLE; owner.sram_saved=0; owner.detached=0; owner.last_error=0;
    return 0;
}
int usb_platform_release(void)
{
    if(owner.state==USB_PLATFORM_IDLE) return 0;
    if(owner.state==USB_PLATFORM_FAULT) return owner.last_error?owner.last_error:USB_PLATFORM_E_RESTORE;
    if(!bus_valid() || (owner.guard_installed && R32(TIMER_VECTOR)!=TIMER_GUARD_ADDRESS)) {
        owner.state=USB_PLATFORM_FAULT; return owner.last_error=USB_PLATFORM_E_LOST;
    }
    if(owner.controller_changed) {
        if(!owner.detached) {
            int result=quiesce_usb();
            if(result) {owner.state=USB_PLATFORM_FAULT;return owner.last_error=result;}
            owner.detached=1;
        }
        if(connected()) { owner.state=USB_PLATFORM_WAIT_DETACH; return 1; }
    }
    return restore_native();
}
int usb_platform_finish_release(unsigned detached_ms)
{
    if(owner.state==USB_PLATFORM_IDLE) return 0;
    if(owner.state==USB_PLATFORM_FAULT) return owner.last_error?owner.last_error:USB_PLATFORM_E_RESTORE;
    if(owner.state!=USB_PLATFORM_WAIT_DETACH || !owner.detached) return USB_PLATFORM_E_BUSY;
    if(connected() && detached_ms<USB_PLATFORM_SOFT_DETACH_MS) return 1;
    return restore_native();
}
int usb_platform_get_state(void) { return owner.state; }
int usb_platform_get_last_error(void) { return owner.last_error; }
