/* Native OS/register/FIFO model for the actual production takeover layer.
 * This proves cleanup sequencing and ownership rules, not USB electrical I/O.
 */
#include <string.h>
#define USB_PLATFORM_TEST 1
#include "../device/usb_platform.c"

static u8 memory[0x02800000u],shared_sram[1024],initial_sram[1024];
static unsigned descriptor_address,descriptor_dummy,sram_cycle,sram_reads;
static unsigned calls[5],writes,mmio_reads,status_reads,usb_writes,usb_reads;
static int failed_operation,no_suspend_effect,lock_depth,drop_sram_write,corrupt_cycle,cable_at_read;
static unsigned misc_key,misc_attempts,misc_ignored,misc_unlocked,nested_locks;
static unsigned competitor_pending,competitor_runs,competitor_key_seen,drop_misc_attempt;
static unsigned fail_after_calls,drop_usb_quiesce;
static unsigned powerup_calls,powerup_violation,powerup_read_count,powerup_write_count;
static int powerup_result;
static unsigned drop_clock_at_lock;

static void set32(u32 a,u32 n)
{
    memory[a]=(u8)n; memory[a+1]=(u8)(n>>8);
    memory[a+2]=(u8)(n>>16); memory[a+3]=(u8)(n>>24);
}
u32 usb_platform_test_read32(u32 a)
{
    if(a+4u>sizeof(memory)) return 0;
    return (u32)memory[a]|((u32)memory[a+1]<<8)|((u32)memory[a+2]<<16)|((u32)memory[a+3]<<24);
}
u8 usb_platform_test_read8(u32 a)
{
    u8 value;
    if(a>=USB && a<USB+256u) usb_reads++;
    if(a<0x02000000u) mmio_reads++;
    if(a==USB+0x22u) {
        status_reads++;
        if(cable_at_read && status_reads>=(unsigned)cable_at_read) memory[a]|=0x80;
    }
    if(a==USB+0x8fu) {
        if(descriptor_dummy) { descriptor_dummy=0; return 0xa5; }
        value=shared_sram[descriptor_address++&1023u];sram_reads++;
        if(corrupt_cycle && sram_cycle==(unsigned)corrupt_cycle) value^=1;
        return value;
    }
    return a<sizeof(memory)?memory[a]:0;
}
void usb_platform_test_write8(u32 a,u8 value)
{
    writes++;
    if(a>=USB && a<USB+256u) usb_writes++;
    if(a>=sizeof(memory)) return;
    if(drop_usb_quiesce && (a==USB+0x21u || a==USB+0x23u)) return;
    if(a==MISC_WRPROT) {
        misc_key=value==0x96u;
        if(competitor_pending) competitor_key_seen=1;
        if(competitor_pending && !lock_depth) {
            /* A native Misc write can consume an unprotected caller's key. */
            competitor_pending=0;competitor_runs++;misc_key=0;
            memory[0x00300f30u]^=1u;
        }
        memory[a]=value;return;
    }
    if(a==MISC || (a>=0x00300f30u && a<=0x00300f39u)) {
        if(a==MISC) misc_attempts++;
        if(!misc_key) {misc_ignored++;return;}
        misc_key=0;misc_unlocked++;
        if(a==MISC && drop_misc_attempt==misc_attempts) return;
        memory[a]=a==MISC?value&0x37u:value;return;
    }
    if(a==USB+0x88u) {
        descriptor_address=((unsigned)(value&15u)<<8)|(descriptor_address&255u);
        descriptor_dummy=1;
    } else if(a==USB+0x89u) {
        descriptor_address=(descriptor_address&0xf00u)|value;
        descriptor_dummy=1;sram_cycle++;
    } else if(a==USB+0x8fu) {
        if(!drop_sram_write) shared_sram[descriptor_address&1023u]=value;
        descriptor_address++;return;
    } else if(a==TIMER_FLAG || a==USB_FLAG) {
        memory[a]&=(u8)~value;return;
    }
    /* USBSNZ only gates/synchronizes access; it resets no USB register/SRAM. */
    memory[a]=value;
}
void usb_platform_test_write32(u32 a,u32 value) {writes++;set32(a,value);}
int usb_platform_test_syscall(unsigned operation,int argument)
{
    (void)argument; calls[operation]++;
    if(failed_operation==(int)operation && calls[operation]>fail_after_calls) return -9;
    if(operation==SYS_LOCK) {
        if(calls[operation]==drop_clock_at_lock) memory[MISC]=0x27;
        if(lock_depth) nested_locks++;
        lock_depth++;return 0;
    }
    if(operation==SYS_UNLOCK) {
        lock_depth--;
        if(!lock_depth && competitor_pending && competitor_key_seen) {
            competitor_pending=0;competitor_runs++;
            /* Native code obtains its own key after this helper's target. */
            usb_platform_test_write8(MISC_WRPROT,0x96);
            usb_platform_test_write8(0x00300f30u,memory[0x00300f30u]^1u);
        }
        return 0;
    }
    if(operation==SYS_SUSPEND) {
        if(!no_suspend_effect) memory[owner.tcb+3u*72u+0x3fu]|=8;
        return 0;
    }
    memory[owner.tcb+3u*72u+0x3fu]&=(u8)~8u;return 0;
}
int usb_platform_test_powerup_delay(void)
{
    powerup_calls++;
    powerup_read_count=usb_reads;powerup_write_count=usb_writes;
    if(lock_depth || (memory[MISC]&0x30u)!=0x30u) powerup_violation++;
    /* No real MMIO or wall-clock waiting in native GCC unit tests. */
    return powerup_result;
}

void model_reset(void)
{
    unsigned i;
    memset(memory,0,sizeof(memory));memset(&owner,0,sizeof(owner));
    memset(calls,0,sizeof(calls));
    writes=mmio_reads=status_reads=sram_cycle=sram_reads=usb_writes=usb_reads=0;
    descriptor_address=descriptor_dummy=0;
    failed_operation=no_suspend_effect=lock_depth=drop_sram_write=corrupt_cycle=cable_at_read=0;
    misc_key=misc_attempts=misc_ignored=misc_unlocked=nested_locks=0;
    competitor_pending=competitor_runs=competitor_key_seen=drop_misc_attempt=0;usb_platform_misc_failure=0;
    fail_after_calls=drop_usb_quiesce=0;
    powerup_calls=powerup_violation=powerup_read_count=powerup_write_count=0;powerup_result=0;
    drop_clock_at_lock=0;
    for(i=0;i<6;i++) set32(0x02000200u+i*4u,profiles[1].tables[i]);
    for(i=0;i<3;i++) memcpy(memory+profiles[1].signature_addresses[i],profiles[1].signatures[i],40);
    set32(0x020000a8u,0x0219c9acu);
    set32(0x021e81e8u+9u*4u,0x021cfe60u);set32(0x021e81e8u+10u*4u,0x021cfed2u);
    set32(0x021e81e8u+23u*4u,0x021ce8aeu);set32(0x021e81e8u+24u*4u,0x021ce8d8u);
    set32(0x022a9820u,0x02068b34u);set32(0x022a9864u,0x02068b34u);set32(0x022a9868u,0x02068b34u);
    set32(0x02068b34u+3u*72u+0x18u,0x0219c1f2u);
    set32(0x02068b34u+3u*72u+0x14u,0x0202a394u);
    memory[0x02068b34u+3u*72u+0x3du]=6;memory[0x02068b34u+3u*72u+0x3fu]=7;
    set32(0x0204979cu,0x00040000u);
    memory[0x48133]=0x13;memory[0x48132]=0;memory[0x48124]=0x42;memory[0x4812e]=9;
    memory[MISC]=0x27;memory[TIMER_ENABLE]=0x44;memory[USB_ENABLE]=0x40;
    memory[USB+0x20]=0x12;memory[USB+0x22]=0x40;memory[USB+0x23]=0x81;
    for(i=0;i<1024;i++) initial_sram[i]=shared_sram[i]=(u8)(i*71u+17u);
}
void model_write8(u32 a,u8 v) { memory[a]=v; }
void model_write32(u32 a,u32 v) { set32(a,v); }
u32 model_read32(u32 a) { return usb_platform_test_read32(a); }
unsigned model_read8(u32 a) { return memory[a]; }
unsigned model_writes(void) { return writes; }
unsigned model_usb_writes(void) {return usb_writes;}
unsigned model_mmio_reads(void) { return mmio_reads; }
unsigned model_calls(unsigned n) { return calls[n]; }
int model_lock_depth(void) { return lock_depth; }
void model_fail_operation(int op) { failed_operation=op; }
void model_no_suspend_effect(void) { no_suspend_effect=1; }
void model_cable_race(int read_number) { cable_at_read=read_number; }
void model_disconnect(void) { cable_at_read=0;memory[USB+0x22]&=0x7f; }
void model_corrupt_cycle(int cycle) { corrupt_cycle=cycle; }
void model_drop_sram_write(void) { drop_sram_write=1; }
void model_modify_cdc(void)
{
    unsigned i;
    for(i=0;i<1024;i++) shared_sram[i]=(u8)(i^0xd8u);
    memory[USB+0x21]=1;memory[USB+0x23]=0x81;
    memory[USB+0x52]=0xa1;memory[USB+0x56]=0x22;
}
int model_sram_restored(void) { return !memcmp(shared_sram,initial_sram,1024); }
void model_protected_write8(u32 a,u8 v) {usb_platform_test_write8(a,v);}
void model_misc_competitor(void) {competitor_pending=1;}
void model_drop_misc_attempt(unsigned n) {drop_misc_attempt=n;}
unsigned model_misc_attempts(void) {return misc_attempts;}
unsigned model_misc_ignored(void) {return misc_ignored;}
unsigned model_misc_unlocked(void) {return misc_unlocked;}
unsigned model_competitor_runs(void) {return competitor_runs;}
unsigned model_nested_locks(void) {return nested_locks;}
unsigned model_misc_failure(void) {return usb_platform_misc_failure;}
void model_fail_operation_after(unsigned op,unsigned successful_calls)
{failed_operation=(int)op;fail_after_calls=successful_calls;}
void model_drop_usb_quiesce(void) {drop_usb_quiesce=1;}
unsigned model_powerup_calls(void) {return powerup_calls;}
unsigned model_powerup_violations(void) {return powerup_violation;}
unsigned model_powerup_usb_reads(void) {return powerup_read_count;}
unsigned model_powerup_usb_writes(void) {return powerup_write_count;}
void model_powerup_failure(int result) {powerup_result=result;}
void model_drop_clock_at_lock(unsigned n) {drop_clock_at_lock=n;}
