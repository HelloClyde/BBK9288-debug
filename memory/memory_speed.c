#include "memory_speed.h"
typedef char speed_word_is_32[(sizeof(unsigned)==4u)?1:-1];
typedef char speed_result_is_96[(sizeof(d88_memory_speed_result)==96u)?1:-1];

#ifdef D88_MEMORY_SPEED_TEST
extern unsigned char d88_speed_test_read8(unsigned address);
extern unsigned short d88_speed_test_read16(unsigned address);
extern unsigned d88_speed_test_read32(unsigned address);
extern void d88_speed_test_write32(unsigned address,unsigned value);
extern unsigned d88_speed_test_psr(void);
extern int d88_speed_test_yield(void);
#define R8(a) d88_speed_test_read8(a)
#define R16(a) d88_speed_test_read16(a)
#define R32(a) d88_speed_test_read32(a)
#define W32(a,v) d88_speed_test_write32((a),(v))
#define PSR() d88_speed_test_psr()
#define YIELD() d88_speed_test_yield()
#define DATA_R32(a) R32(a)
#else
#define R8(a) (*(volatile unsigned char *)(a))
#define R16(a) (*(volatile unsigned short *)(a))
#define R32(a) (*(volatile unsigned *)(a))
#define W32(a,v) (*(volatile unsigned *)(a)=(v))
static unsigned current_psr(void)
{
    unsigned psr;
    __asm__ volatile("ld.w %0, %%psr" : "=r"(psr) : : "memory");
    return psr;
}
#define PSR() current_psr()
#define YIELD() ((int(*)(unsigned))0x021cf58eu)(1u)
static unsigned data_read32(unsigned address)
{
    unsigned value;
    /* IRAM legitimately begins at 0. A C null dereference would be undefined;
     * this fixed instruction reads address 0 without relying on C UB flags. */
    __asm__ volatile("ld.w %0, [%1]" : "=r"(value) : "r"(address) : "memory");
    return value;
}
#define DATA_R32(a) data_read32(a)
#endif

enum { RTC_PHASE=0x40153u,RTC_SECOND=0x40154u,ROS_TICK=0x02068674u };
static int finish(d88_memory_speed_result *out,int result)
{ out->error=(unsigned)result;return result; }
static int range(unsigned start,unsigned size,unsigned address,unsigned bytes)
{ return address>=start && address-start<=size && bytes<=size-(address-start); }
static int sdram(unsigned address,unsigned bytes)
{ return !(address&3u) && range(0x02000000u,0x00800000u,address,bytes); }
static int allowed(unsigned operation,unsigned source,unsigned destination,unsigned bytes,
                   int (*owns)(unsigned,unsigned))
{
    if(operation==D88_MEMORY_SPEED_READ) {
        if(destination || (source&3u) || (bytes!=16384u && bytes!=65536u))
            return D88_MEMORY_SPEED_E_ARGUMENT;
        if(sdram(source,bytes) || (bytes==16384u &&
           (range(0,16384u,source,bytes) || range(0x003c0000u,40960u,source,bytes)))) return 0;
        return D88_MEMORY_SPEED_E_ARGUMENT;
    }
    if((operation!=D88_MEMORY_SPEED_WRITE && operation!=D88_MEMORY_SPEED_COPY) ||
       bytes!=65536u || !sdram(destination,bytes)) return D88_MEMORY_SPEED_E_ARGUMENT;
    if(operation==D88_MEMORY_SPEED_WRITE && source) return D88_MEMORY_SPEED_E_ARGUMENT;
    if(operation==D88_MEMORY_SPEED_COPY &&
       (!sdram(source,bytes) || (source<destination+bytes && destination<source+bytes)))
        return D88_MEMORY_SPEED_E_ARGUMENT;
    if(!owns || !owns(destination,bytes) ||
       (operation==D88_MEMORY_SPEED_COPY && !owns(source,bytes))) return D88_MEMORY_SPEED_E_OWNER;
    return 0;
}
static int context(void)
{
    if((PSR()&0x710u)!=0x10u || R8(0x0206866cu) || (R8(0x0206866du)&7u))
        return D88_MEMORY_SPEED_E_CONTEXT;
    if(R32(0x02000200u)!=0x021e81e8u ||
       R32(0x021e81e8u+27u*4u)!=0x021cf58eu) return D88_MEMORY_SPEED_E_CONTEXT;
    return 0;
}
static int clock_matches(const d88_memory_speed_result *out)
{
    return (R8(0x40151u)&1u) && R16(0x48192u)==out->t2_compare_b &&
        R8(0x48196u)==out->t2_ctrl && R8(0x40149u)==out->t2_clock &&
        R8(0x40181u)==out->psc_select && R8(0x40180u)==out->psc_power &&
        R8(0x40190u)==out->clock_option;
}
static int rtc_stamp(unsigned *stamp)
{
    unsigned i,first,phase,last,minute_first,minute_last;
    for(i=0;i<8u;i++) {
        minute_first=R8(0x40155u);
        first=R8(RTC_SECOND);phase=R8(RTC_PHASE);last=R8(RTC_SECOND);
        minute_last=R8(0x40155u);
        if(first==last && minute_first==minute_last && first<60u && minute_first<60u) {
            *stamp=(minute_first*60u+first)*256u+phase;return 0;
        }
    }
    return D88_MEMORY_SPEED_E_COHERENCE;
}
static unsigned elapsed(unsigned start,unsigned end)
{ return end>=start?end-start:D88_MEMORY_SPEED_RTC_MODULUS-start+end; }

/* No inlining or per-bank specialization: inspect the generated target
 * assembly for these same volatile 32-bit loops at one code location. */
static unsigned __attribute__((noinline)) read_kernel(unsigned source,unsigned bytes)
{
    unsigned i,sum=0;
    for(i=0;i<bytes;i+=4u) sum+=DATA_R32(source+i);
    return sum;
}
static unsigned __attribute__((noinline)) write_kernel(unsigned destination,unsigned bytes)
{
    unsigned i;
    for(i=0;i<bytes;i+=4u) W32(destination+i,D88_MEMORY_SPEED_WRITE_WORD);
    return R32(destination+bytes-4u);
}
static unsigned __attribute__((noinline)) copy_kernel(unsigned source,unsigned destination,unsigned bytes)
{
    unsigned i;
    for(i=0;i<bytes;i+=4u) W32(destination+i,DATA_R32(source+i));
    return R32(destination+bytes-4u);
}
int d88_memory_speed_run(unsigned operation,unsigned source,unsigned destination,
    unsigned bytes,int (*owns)(unsigned,unsigned),d88_memory_speed_result *out)
{
    unsigned i,window,start,end,start_ros,end_ros,passes,max_passes,delta,check;
    int result;
    if(!out) return D88_MEMORY_SPEED_E_ARGUMENT;
    for(i=0;i<sizeof(*out)/sizeof(unsigned);i++) ((unsigned*)out)[i]=0;
    out->version=D88_MEMORY_SPEED_VERSION;out->operation=operation;
    out->source=source;out->destination=destination;out->bytes_per_pass=bytes;
    out->rtc_nominal_hz=256u;
    out->flags=D88_MEMORY_SPEED_FLAG_NOMINAL_RTC|D88_MEMORY_SPEED_FLAG_YIELD_EXCLUDED;
    result=allowed(operation,source,destination,bytes,owns);
    if(result) return finish(out,result);
    result=context();if(result) return finish(out,result);
    out->flags|=D88_MEMORY_SPEED_FLAG_INTERRUPTS;
    out->t2_compare_b=R16(0x48192u);out->t2_ctrl=R8(0x48196u);
    out->t2_clock=R8(0x40149u);out->psc_select=R8(0x40181u);
    out->psc_power=R8(0x40180u);out->clock_option=R8(0x40190u);
    if(!clock_matches(out) || out->t2_compare_b!=0x124u ||
       out->t2_ctrl!=1u || out->t2_clock!=0x0fu || (out->psc_select&1u) ||
       !(out->psc_power&0x20u)) return finish(out,D88_MEMORY_SPEED_E_CLOCK);
    /* Confirm RTC progress before touching writable payload. This yield is
     * outside every measured window. Never enable/reset a stopped timer. */
    result=rtc_stamp(&start);if(result) return finish(out,result);
    if(YIELD()!=0) return finish(out,D88_MEMORY_SPEED_E_YIELD);
    result=rtc_stamp(&end);if(result) return finish(out,result);
    if(!elapsed(start,end)) return finish(out,D88_MEMORY_SPEED_E_STALLED);
    out->t2_first=R16(0x48194u);out->t2_last=out->t2_first;
    /* Independent finite work bound: 32MiB per window even if both clocks
     * stop. A 32-ROS-unit watchdog adds an earlier bound when ROS still runs;
     * ROS units are never used to compute reported memory throughput. */
    max_passes=bytes==16384u?2048u:512u;
    for(window=0;window<D88_MEMORY_SPEED_WINDOWS;window++) {
        result=allowed(operation,source,destination,bytes,owns);
        if(result) return finish(out,result);
        result=context();if(result) return finish(out,result);
        if(!clock_matches(out)) return finish(out,D88_MEMORY_SPEED_E_CHANGED);
        result=rtc_stamp(&start);if(result) return finish(out,result);
        if(!window) out->rtc_first=start;
        start_ros=R32(ROS_TICK);passes=0;
        do {
            if(operation==D88_MEMORY_SPEED_READ) check=read_kernel(source,bytes);
            else if(operation==D88_MEMORY_SPEED_WRITE) check=write_kernel(destination,bytes);
            else check=copy_kernel(source,destination,bytes);
            out->checksum=((out->checksum<<1)|(out->checksum>>31))+check;
            passes++;out->passes++;out->bytes_processed+=bytes;
            out->t2_last=R16(0x48194u);
            if(out->t2_last!=out->t2_first) out->flags|=D88_MEMORY_SPEED_FLAG_TIMER2_CHANGED;
            result=rtc_stamp(&end);if(result) return finish(out,result);
            delta=elapsed(start,end);end_ros=R32(ROS_TICK);
            if(delta>2560u) return finish(out,D88_MEMORY_SPEED_E_CHANGED);
            if(delta<D88_MEMORY_SPEED_WINDOW_TICKS &&
               (passes>=max_passes || end_ros-start_ros>=32u)) {
                /* Give the background debugger a chance to service USB even
                 * on a failed timing source. Keep elapsed results untrusted. */
                if(YIELD()!=0) return finish(out,D88_MEMORY_SPEED_E_YIELD);
                return finish(out,D88_MEMORY_SPEED_E_STALLED);
            }
        } while(delta<D88_MEMORY_SPEED_WINDOW_TICKS);
        out->rtc_ticks+=delta;out->ros_ticks+=end_ros-start_ros;
        out->rtc_last=end;out->windows++;
        if(!clock_matches(out)) return finish(out,D88_MEMORY_SPEED_E_CHANGED);
        if(window+1u<D88_MEMORY_SPEED_WINDOWS && YIELD()!=0)
            return finish(out,D88_MEMORY_SPEED_E_YIELD);
    }
    return finish(out,0);
}
int d88_memory_speed_clock(unsigned *rtc_ticks,unsigned *ros_ticks)
{
    unsigned stamp;
    int result;
    if(!rtc_ticks || !ros_ticks) return D88_MEMORY_SPEED_E_ARGUMENT;
    result=context();if(result) return result;
    if(!(R8(0x40151u)&1u)) return D88_MEMORY_SPEED_E_CLOCK;
    result=rtc_stamp(&stamp);if(result) return result;
    *rtc_ticks=stamp;*ros_ticks=R32(ROS_TICK);
    return 0;
}
