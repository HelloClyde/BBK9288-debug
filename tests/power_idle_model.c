/* Production C model: a native idle timer and a pure aligned-store reset.
 * This is not a substitute for native timer/GUI shutdown integration. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define D88_POWER_IDLE_TEST 1
#include "../device/power_idle.c"

static unsigned root,entry,idle_count,setting,msc_flag,timers[16];
static unsigned char code[10];
static unsigned resets,reads,expired;
static unsigned ros_root,lock_entry,unlock_entry,pending,cpu_depth,dispatch_flags,psr;
static unsigned locked,locks,unlocks,lock_error,unlock_error,lock_race;
static unsigned irq_before_lock,irq_after_unlock;

unsigned d88_power_test_read32(unsigned address)
{
    reads++;
    if(address==GUI_ROOT) return root;
    if(address==ROS_ROOT) return ros_root;
    if(address==ROS_TABLE+23u*4u) return lock_entry;
    if(address==ROS_TABLE+24u*4u) return unlock_entry;
    if(address==PENDING_REQUEST) {assert(locked);return pending;}
    assert(address==RESET_SLOT);return entry;
}
unsigned char d88_power_test_read8(unsigned address)
{
    reads++;
    if(address==0x0206866cu) return (unsigned char)cpu_depth;
    if(address==0x0206866du) return (unsigned char)dispatch_flags;
    assert(address>=RESET_ENTRY && address<RESET_ENTRY+sizeof(code));
    return code[address-RESET_ENTRY];
}
void d88_power_test_reset(void)
{
    assert(locked && !pending && (dispatch_flags&3u)==3u && !(psr&0x10u));
    resets++;
    idle_count=0; /* Exact native leaf semantics; no settings/USB/timer writes. */
}
unsigned d88_power_test_psr(void) {return psr;}
int d88_power_test_lock(void)
{
    locks++;assert(!locked && !cpu_depth && !(dispatch_flags&7u) && (psr&0x710u)==0x10u);
    if(lock_error) return -1;
    if(irq_before_lock) idle_count++;
    locked=1;dispatch_flags=3;psr&=~0x10u;
    /* Model an interrupt/task change immediately before the lock takes
     * effect, after the caller's initial profile/context checks. */
    if(lock_race==1u) pending=1;
    else if(lock_race==2u) root+=4u;
    else if(lock_race==3u) entry+=2u;
    else if(lock_race==4u) code[9]^=1;
    else if(lock_race==5u) lock_entry+=2u;
    return 0;
}
int d88_power_test_unlock(void)
{
    unlocks++;assert(locked);
    if(unlock_error) return -1;
    locked=0;dispatch_flags=0;psr|=0x10u;
    if(irq_after_unlock) idle_count++;
    return 0;
}
static void setup(void)
{
    static const unsigned char captured[10]={0x04,0x6c,0x77,0xd2,0x54,0xcf,0xf4,0x3c,0x40,0x06};
    unsigned i;
    root=GUI_TABLE;entry=RESET_ENTRY;memcpy(code,captured,sizeof(code));
    idle_count=91;setting=2;msc_flag=0;
    for(i=0;i<16u;i++) timers[i]=100u+i;
    resets=reads=expired=0;irq_before_lock=irq_after_unlock=0;
    ros_root=ROS_TABLE;lock_entry=LOCK_ENTRY;unlock_entry=UNLOCK_ENTRY;
    pending=cpu_depth=dispatch_flags=locked=locks=unlocks=lock_error=unlock_error=lock_race=0;
    psr=0x10u;
    power_idle_diagnostic.version=D88_POWER_IDLE_DIAG_VERSION;
    power_idle_diagnostic.active=power_idle_diagnostic.refreshes=power_idle_diagnostic.last_error=0;
}
static int checked_begin(void)
{
    unsigned old_setting=setting,old_flag=msc_flag,saved[16];
    int result;
    memcpy(saved,timers,sizeof(saved));result=d88_power_idle_begin();
    assert(setting==old_setting && msc_flag==old_flag && !memcmp(saved,timers,sizeof(saved)));
    return result;
}
static int checked_poll(void)
{
    unsigned old_setting=setting,old_flag=msc_flag,saved[16];
    int result;
    memcpy(saved,timers,sizeof(saved));result=d88_power_idle_poll();
    assert(setting==old_setting && msc_flag==old_flag && !memcmp(saved,timers,sizeof(saved)));
    return result;
}
static void tick(void)
{
    unsigned i;
    idle_count++;
    for(i=0;i<16u;i++) timers[i]++;
    /* The real ISR compares count*4 against minutes*60 (about4s per count),
     * independently of ROS ticks. A new expiry raises a pending request. */
    if(idle_count*4u==setting*60u) {expired++;pending=1;}
}
static void test_lifecycle(void)
{
    unsigned i,timeout,before;
    setup();assert(!checked_poll() && !reads && !resets);
    assert(!checked_begin() && power_idle_diagnostic.active && !idle_count);
    timeout=setting*15u;
    for(i=0;i<timeout*3u;i++) {
        tick();
        if(i%7u==0u) assert(!checked_poll());
    }
    assert(!expired && idle_count<7u && setting==2u && !msc_flag);
    assert(timers[0]==100u+timeout*3u && timers[15]==115u+timeout*3u);
    before=resets;d88_power_idle_end();assert(!power_idle_diagnostic.active);
    for(i=0;i<timeout;i++) {tick();assert(!checked_poll());}
    assert(expired==1u && resets==before);
    assert(power_idle_diagnostic.refreshes==resets);
    assert(checked_begin()==D88_POWER_IDLE_E_PENDING && resets==before);
    pending=0; /* The native GUI consumes its request outside this module. */
    assert(!checked_begin() && power_idle_diagnostic.active && !idle_count);
    assert(power_idle_diagnostic.refreshes==before+1u);
    d88_power_idle_end();
}
static void test_bad_profiles(void)
{
    unsigned i,before,before_reads;
    setup();root=GUI_TABLE+4u;
    assert(checked_begin()==D88_POWER_IDLE_E_TABLE && !resets && idle_count==91u);
    assert(!power_idle_diagnostic.active && (int)power_idle_diagnostic.last_error==D88_POWER_IDLE_E_TABLE);
    setup();entry=RESET_ENTRY+2u;
    assert(checked_begin()==D88_POWER_IDLE_E_ENTRY && !resets);
    for(i=0;i<sizeof(code);i++) {
        setup();code[i]^=1;
        assert(checked_begin()==D88_POWER_IDLE_E_CODE && !resets && idle_count==91u);
        assert(!power_idle_diagnostic.active && (int)power_idle_diagnostic.last_error==D88_POWER_IDLE_E_CODE);
    }
    for(i=0;i<3u;i++) {
        setup();assert(!checked_begin());before=resets;idle_count=20;
        if(i==0) root=GUI_TABLE+4u;
        else if(i==1) entry=RESET_ENTRY+2u;
        else code[8]^=1;
        assert(checked_poll()==-(int)(401u+i));
        assert(!power_idle_diagnostic.active && idle_count==20u && resets==before);
        before_reads=reads;assert(!checked_poll());
        assert(reads==before_reads && resets==before);
        d88_power_idle_end();
        assert((int)power_idle_diagnostic.last_error==-(int)(401u+i));
    }
    setup();ros_root+=4u;assert(checked_begin()==D88_POWER_IDLE_E_ROS && !locks && !resets);
    setup();lock_entry+=2u;assert(checked_begin()==D88_POWER_IDLE_E_ROS && !locks && !resets);
    setup();unlock_entry+=2u;assert(checked_begin()==D88_POWER_IDLE_E_ROS && !locks && !resets);
}
static void test_irq_near_store(void)
{
    setup();irq_before_lock=1;
    assert(!checked_begin() && !idle_count);
    irq_before_lock=0;irq_after_unlock=1;
    assert(!checked_poll() && idle_count==1u && !power_idle_diagnostic.last_error);
    irq_before_lock=1;
    assert(!checked_poll() && idle_count==1u);
    assert(power_idle_diagnostic.refreshes==3u);
    d88_power_idle_end();
}
static void test_locks_and_pending(void)
{
    unsigned i,before;
    static const int race_errors[]={0,D88_POWER_IDLE_E_PENDING,D88_POWER_IDLE_E_TABLE,
        D88_POWER_IDLE_E_ENTRY,D88_POWER_IDLE_E_CODE,D88_POWER_IDLE_E_ROS};
    setup();pending=1;
    assert(checked_begin()==D88_POWER_IDLE_E_PENDING && pending==1u && idle_count==91u);
    assert(!resets && locks==1u && unlocks==1u && !locked);
    setup();assert(!checked_begin());pending=1;before=resets;
    assert(checked_poll()==D88_POWER_IDLE_E_PENDING && !power_idle_diagnostic.active);
    assert(resets==before && pending==1u && !locked);
    for(i=1;i<=5u;i++) {
        setup();lock_race=i;
        assert(checked_begin()==race_errors[i]);
        assert(!resets && idle_count==91u && !power_idle_diagnostic.active);
        assert(locks==1u && unlocks==1u && !locked);
        assert(pending==(i==1u));
    }
    for(i=0;i<7u;i++) {
        setup();
        if(i==0u) cpu_depth=1;
        else if(i<=3u) dispatch_flags=1u<<(i-1u);
        else if(i==4u) psr=0;
        else if(i==5u) psr=0x110u;
        else psr=0x710u;
        assert(checked_begin()==D88_POWER_IDLE_E_CONTEXT && !locks && !unlocks && !resets);
    }
    setup();lock_error=1;
    assert(checked_begin()==D88_POWER_IDLE_E_LOCK && !resets && locks==1u && !unlocks);
    setup();unlock_error=1;
    assert(checked_begin()==D88_POWER_IDLE_E_UNLOCK && resets==1u && locked);
    assert(!power_idle_diagnostic.active && (int)power_idle_diagnostic.last_error==D88_POWER_IDLE_E_UNLOCK);
    setup();pending=1;unlock_error=1;
    assert(checked_begin()==D88_POWER_IDLE_E_UNLOCK && !resets && pending==1u);
    setup();assert(!checked_begin());lock_error=1;before=resets;
    assert(checked_poll()==D88_POWER_IDLE_E_LOCK && resets==before && !power_idle_diagnostic.active);
    setup();assert(!checked_begin());unlock_error=1;before=resets;
    assert(checked_poll()==D88_POWER_IDLE_E_UNLOCK && resets==before+1u && !power_idle_diagnostic.active);
}
int main(void)
{
    assert(sizeof(d88_power_idle_diagnostic)==16u);
    test_lifecycle();test_bad_profiles();test_irq_near_store();test_locks_and_pending();
    puts("power idle production model: PASS");return 0;
}
