#include <assert.h>
#include <stdio.h>
#include <string.h>
#define D88_MEMORY_SPEED_TEST 1
#include "../memory/memory_speed.c"

#define SRC 0x02580000u
#define DST 0x025a0000u
static unsigned source_words[16384],destination_words[16384];
static unsigned long long quanta;
static unsigned origin,reads,writes,yields,owner_calls,deny_after;
static unsigned psr,depth,locks,ros_root,delay_entry,run,clock_ctrl,compare,ctrl;
static unsigned frozen,freeze_during,freeze_ros,unstable,carry_once,change_on_yield,fail_yield;
static unsigned rtc_seconds_reads,frozen_stamp;
static unsigned rtc_now(void)
{ return (origin+(unsigned)(quanta/8192u))%D88_MEMORY_SPEED_RTC_MODULUS; }
static void operation(void)
{
    quanta++;
    if(!frozen && freeze_during && reads+writes>=freeze_during) {
        frozen_stamp=rtc_now();frozen=1;
    }
}
unsigned char d88_speed_test_read8(unsigned address)
{
    unsigned stamp=rtc_now();
    if(address==0x0206866cu) return (unsigned char)depth;
    if(address==0x0206866du) return (unsigned char)locks;
    if(address==0x40151u) return (unsigned char)run;
    if(address==0x40149u) return (unsigned char)clock_ctrl;
    if(address==0x48196u) return (unsigned char)ctrl;
    if(address==0x40181u || address==0x40190u) return 0;
    if(address==0x40180u) return 0x20;
    if(frozen) stamp=frozen_stamp;
    if(address==RTC_SECOND) {
        if(unstable) return (unsigned char)(rtc_seconds_reads++&1u);
        return (unsigned char)((stamp/256u)%60u);
    }
    if(address==0x40155u) return (unsigned char)(stamp/(60u*256u));
    assert(address==RTC_PHASE);
    if(carry_once) {quanta+=8192u;carry_once=0;stamp=rtc_now();}
    return (unsigned char)stamp;
}
unsigned short d88_speed_test_read16(unsigned address)
{
    if(address==0x48192u) return (unsigned short)compare;
    assert(address==0x48194u);
    return (unsigned short)((quanta/64u)%293u);
}
unsigned d88_speed_test_read32(unsigned address)
{
    unsigned index;
    if(address==0x02000200u) return ros_root;
    if(address==0x021e81e8u+27u*4u) return delay_entry;
    if(address==ROS_TICK) return freeze_ros?0u:(unsigned)(quanta/(8192u*6u));
    assert(!(address&3u));reads++;operation();
    if(address>=DST && address<DST+65536u) return destination_words[(address-DST)/4u];
    assert((address<16384u) || (address>=0x003c0000u && address<0x003ca000u) ||
           (address>=0x02000000u && address<0x02800000u));
    index=(address-SRC)/4u;return source_words[index&16383u];
}
void d88_speed_test_write32(unsigned address,unsigned value)
{
    assert(address>=DST && address<DST+65536u && !(address&3u));
    writes++;operation();destination_words[(address-DST)/4u]=value;
}
unsigned d88_speed_test_psr(void) {return psr;}
int d88_speed_test_yield(void)
{
    yields++;
    if(fail_yield==yields) return -1;
    quanta+=8192u*7u;
    if(change_on_yield==yields) clock_ctrl^=1u;
    return 0;
}
static int owns(unsigned address,unsigned bytes)
{
    owner_calls++;
    if(deny_after && owner_calls>deny_after) return 0;
    return bytes==65536u && (address==SRC || address==DST);
}
static void setup(void)
{
    unsigned i;
    quanta=0;origin=0;reads=writes=yields=owner_calls=deny_after=0;
    psr=0x10;depth=locks=0;ros_root=0x021e81e8u;delay_entry=0x021cf58eu;
    run=ctrl=1;clock_ctrl=0xf;compare=0x124;
    frozen=freeze_during=freeze_ros=unstable=carry_once=change_on_yield=fail_yield=0;
    rtc_seconds_reads=frozen_stamp=0;
    for(i=0;i<16384u;i++) {source_words[i]=i*73u+19u;destination_words[i]=~source_words[i];}
}
static void test_operations(void)
{
    d88_memory_speed_result out;
    unsigned i,clock_rtc,clock_ros;
    static const unsigned read_sources[]={0,0x003c0000u,0x02010000u,0x02210000u,0x02410000u,0x02610000u};
    for(i=0;i<sizeof(read_sources)/sizeof(read_sources[0]);i++) {
        setup();assert(!d88_memory_speed_run(D88_MEMORY_SPEED_READ,read_sources[i],0,16384u,0,&out));
        assert(out.source==read_sources[i] && out.windows==4u && !out.error);
        assert(out.rtc_ticks==128u && out.passes==256u && out.bytes_processed==4194304u);
        assert(reads==out.bytes_processed/4u && !writes && !owner_calls && yields==4u);
        assert(rtc_now()==out.rtc_ticks+7u*yields); /* Explicit yield time excluded. */
        assert(out.flags==15u && out.rtc_nominal_hz==256u);
    }
    setup();assert(!d88_memory_speed_run(D88_MEMORY_SPEED_READ,SRC,0,65536u,0,&out));
    assert(out.bytes_processed==out.passes*65536u && !writes);
    setup();assert(!d88_memory_speed_run(D88_MEMORY_SPEED_WRITE,0,DST,65536u,owns,&out));
    assert(writes==out.bytes_processed/4u && reads==out.passes && owner_calls==5u);
    for(i=0;i<16384u;i++) assert(destination_words[i]==D88_MEMORY_SPEED_WRITE_WORD);
    setup();assert(!d88_memory_speed_run(D88_MEMORY_SPEED_COPY,SRC,DST,65536u,owns,&out));
    assert(writes==out.bytes_processed/4u && reads==writes+out.passes && owner_calls==10u);
    assert(!memcmp(source_words,destination_words,sizeof(source_words)));
    assert(out.ros_ticks<((unsigned)(quanta/(8192u*6u))));
    assert(!d88_memory_speed_clock(&clock_rtc,&clock_ros) && clock_rtc==rtc_now());
    assert(clock_ros==(unsigned)(quanta/(8192u*6u)));
}
static void test_arguments_and_ownership(void)
{
    d88_memory_speed_result out;
    setup();assert(d88_memory_speed_run(1,0,0,16384u,0,0)==D88_MEMORY_SPEED_E_ARGUMENT);
    assert(d88_memory_speed_run(1,0x1000000u,0,16384u,0,&out)==D88_MEMORY_SPEED_E_ARGUMENT);
    assert(d88_memory_speed_run(1,0x027ff000u,0,16384u,0,&out)==D88_MEMORY_SPEED_E_ARGUMENT);
    assert(d88_memory_speed_run(1,1,0,16384u,0,&out)==D88_MEMORY_SPEED_E_ARGUMENT);
    assert(d88_memory_speed_run(1,0,0,65536u,0,&out)==D88_MEMORY_SPEED_E_ARGUMENT);
    assert(d88_memory_speed_run(1,SRC,DST,16384u,0,&out)==D88_MEMORY_SPEED_E_ARGUMENT);
    assert(d88_memory_speed_run(2,0,0,65536u,owns,&out)==D88_MEMORY_SPEED_E_ARGUMENT);
    assert(d88_memory_speed_run(2,0,0x003c0000u,65536u,owns,&out)==D88_MEMORY_SPEED_E_ARGUMENT);
    assert(d88_memory_speed_run(2,0,DST,16384u,owns,&out)==D88_MEMORY_SPEED_E_ARGUMENT);
    assert(d88_memory_speed_run(3,SRC,SRC+4u,65536u,owns,&out)==D88_MEMORY_SPEED_E_ARGUMENT);
    assert(d88_memory_speed_run(2,0,DST,65536u,0,&out)==D88_MEMORY_SPEED_E_OWNER);
    assert(d88_memory_speed_run(3,SRC+65536u,DST,65536u,owns,&out)==D88_MEMORY_SPEED_E_OWNER);
    assert(!reads && !writes && !yields);
    setup();deny_after=2;
    assert(d88_memory_speed_run(2,0,DST,65536u,owns,&out)==D88_MEMORY_SPEED_E_OWNER);
    assert(out.windows==1u && writes && yields==2u);
}
static void test_clocks_and_failures(void)
{
    d88_memory_speed_result out;
    unsigned a=11,b=22;
    setup();origin=D88_MEMORY_SPEED_RTC_MODULUS-1u;carry_once=1;
    assert(!d88_memory_speed_clock(&a,&b) && a==0u); /* Coherent minute/hour carry retry. */
    setup();unstable=1;
    assert(d88_memory_speed_run(1,SRC,0,16384u,0,&out)==D88_MEMORY_SPEED_E_COHERENCE);
    assert(!reads && !writes && !yields);
    setup();frozen=1;
    assert(d88_memory_speed_run(2,0,DST,65536u,owns,&out)==D88_MEMORY_SPEED_E_STALLED);
    assert(!writes && !reads && yields==1u);
    setup();freeze_during=1;freeze_ros=1;
    assert(d88_memory_speed_run(1,SRC,0,16384u,0,&out)==D88_MEMORY_SPEED_E_STALLED);
    assert(out.passes==2048u && out.bytes_processed==32u*1024u*1024u && yields==2u);
    setup();run=0;
    assert(d88_memory_speed_run(1,SRC,0,16384u,0,&out)==D88_MEMORY_SPEED_E_CLOCK && !reads);
    setup();compare++;
    assert(d88_memory_speed_run(1,SRC,0,16384u,0,&out)==D88_MEMORY_SPEED_E_CLOCK && !reads);
    setup();psr=0;
    assert(d88_memory_speed_run(1,SRC,0,16384u,0,&out)==D88_MEMORY_SPEED_E_CONTEXT && !reads);
    setup();locks=4;
    assert(d88_memory_speed_run(1,SRC,0,16384u,0,&out)==D88_MEMORY_SPEED_E_CONTEXT && !reads);
    setup();ros_root++;
    assert(d88_memory_speed_run(1,SRC,0,16384u,0,&out)==D88_MEMORY_SPEED_E_CONTEXT && !reads);
    setup();delay_entry++;
    assert(d88_memory_speed_clock(&a,&b)==D88_MEMORY_SPEED_E_CONTEXT);
    setup();fail_yield=2;
    assert(d88_memory_speed_run(1,SRC,0,16384u,0,&out)==D88_MEMORY_SPEED_E_YIELD && out.windows==1u);
    setup();change_on_yield=2;
    assert(d88_memory_speed_run(1,SRC,0,16384u,0,&out)==D88_MEMORY_SPEED_E_CHANGED && out.windows==1u);
    setup();assert(d88_memory_speed_clock(0,&b)==D88_MEMORY_SPEED_E_ARGUMENT);
}
int main(void)
{
    assert(sizeof(d88_memory_speed_result)==96u);
    test_operations();test_arguments_and_ownership();test_clocks_and_failures();
    puts("memory speed production model: PASS");return 0;
}
