/* Production frontend orchestration, fixed-mapped private process RAM only.
 * Heap/timing/map primitives are separately covered production C modules. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#define MEMORY_IMAGE_ID 0x12345678u
#include "../memory/main.c"

static unsigned owner,owner_bytes,release_calls,allocate_calls,probe_calls,speed_calls;
static unsigned clock_value,mode,shows,power_ends;
static int fake_delay(unsigned ticks) {(void)ticks;return 0;}
static int fake_show(unsigned w,const char *s,const char *t,unsigned f)
{(void)w;(void)s;(void)t;(void)f;shows++;return 0;}
static T_GUI_RelocationTable gui={fake_show};
static T_ROS33_RelocationTable ros={fake_delay};
T_GUI_RelocationTable *tpDL_GUITable=&gui;
T_ROS33_RelocationTable *tpDL_ROS33Table=&ros;
volatile d88_power_idle_diagnostic power_idle_diagnostic;
int d88_power_idle_begin(void) {power_idle_diagnostic.active=1;return 0;}
int d88_power_idle_poll(void) {assert(power_idle_diagnostic.active);return 0;}
void d88_power_idle_end(void) {power_idle_diagnostic.active=0;power_ends++;}
int d88_mem_map_capture(d88_mem_map_report *out)
{out->version=1;out->structure_bytes=sizeof(*out);return mode==3u?-4:0;}
int d88_memory_heap_snapshot(d88_heap_snapshot *out)
{out->version=1;out->bytes=sizeof(*out);out->start=0x02500000u;out->end=0x02700000u;return 0;}
int d88_memory_heap_probe(d88_heap_probe *out)
{probe_calls++;out->version=1;out->bytes=sizeof(*out);out->allocated_ok=out->state_restored=out->directory_restored=out->boundary_failed=1;return 0;}
int d88_memory_heap_allocate(unsigned requested,d88_heap_allocation *out)
{
    allocate_calls++;assert(!owner);
    if(mode==4u && requested==0x100000u) return -512;
    owner=0x02550000u;owner_bytes=requested;
    out->version=1;out->bytes=sizeof(*out);out->address=owner;out->allocation_bytes=owner_bytes;out->active=1;return 0;
}
int d88_memory_heap_release(void) {release_calls++;owner=owner_bytes=0;return 0;}
int d88_memory_heap_owns(unsigned a,unsigned n)
{return owner && n && a>=owner && a-owner<=owner_bytes && n<=owner_bytes-(a-owner);}
unsigned d88_memory_heap_fault(void) {return 0;}
int d88_memory_speed_clock(unsigned *rtc,unsigned *ticks)
{clock_value+=100;*rtc=clock_value;*ticks=clock_value*2u;return 0;}
int d88_memory_speed_run(unsigned op,unsigned src,unsigned dst,unsigned bytes,
    int(*owns)(unsigned,unsigned),d88_memory_speed_result *out)
{
    unsigned i;volatile unsigned *s=(volatile unsigned*)(size_t)src,*d=(volatile unsigned*)(size_t)dst;
    speed_calls++;out->version=1;out->operation=op;out->source=src;out->destination=dst;
    out->bytes_per_pass=bytes;out->windows=4;out->passes=2;out->bytes_processed=bytes*2;
    out->rtc_ticks=128;out->rtc_nominal_hz=256;
    if(op==2u) {
        assert(owns(dst,bytes));
        for(i=0;i<bytes/4;i++) d[i]=D88_MEMORY_SPEED_WRITE_WORD;
        if(mode==1u) d[bytes/4-1]^=1;
    } else if(op==3u) {
        assert(owns(src,bytes) && owns(dst,bytes));
        /* Every COPY must start with different, nonconstant source/target. */
        assert(s[0]!=d[0] && s[1]!=s[0] && s[bytes/4-1]!=d[bytes/4-1]);
        if(mode!=2u) for(i=0;i<bytes/4;i++) d[i]=s[i];
    } else assert(op==1u && !dst);
    return 0;
}
static unsigned crc(const volatile unsigned char *p,unsigned n)
{
    unsigned c=0xffffffffu,i,b;
    for(i=0;i<n;i++){c^=p[i];for(b=0;b<8;b++)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}
    return c^0xffffffffu;
}
static void reset(unsigned next_mode)
{
    owner=owner_bytes=release_calls=allocate_calls=probe_calls=speed_calls=shows=power_ends=clock_value=0;
    mode=next_mode;
}
int main(int argc,char **argv)
{
    unsigned i;
#ifdef _WIN32
    void *mapped=VirtualAlloc((void*)(size_t)0x02000000u,0x00800000u,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
#else
    void *mapped=mmap((void*)(size_t)0x02000000u,0x00800000u,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
#endif
    assert(mapped==(void*)(size_t)0x02000000u);
    for(i=0;i<6;i++) *(unsigned*)(size_t)(0x02000200u+i*4)=profiles[1].tables[i];
    for(i=0;i<3;i++) memcpy((void*)(size_t)profiles[1].signature_addresses[i],profiles[1].signatures[i],40);
    assert(profile());
    reset(0);assert(!d88_memory_run());assert(d88_memory_report[4]==3 && d88_memory_report[7]==30 && d88_memory_report[8]==6);
    assert(speed_calls==30 && probe_calls==1 && allocate_calls==1 && release_calls==1 && !owner && !allocation.active && power_ends==1);
    assert(d88_memory_report[3]==MEMORY_IMAGE_ID && d88_memory_report[REPORT_WORDS-1]==crc((volatile unsigned char*)d88_memory_report,(REPORT_WORDS-1)*4));
    assert(buffer_a==0x02550000u && buffer_b==0x02600000u);
    if(argc==2) {
        FILE *file=fopen(argv[1],"wb");assert(file);
        assert(fwrite((const void*)d88_memory_report,1,sizeof(d88_memory_report),file)==sizeof(d88_memory_report));
        assert(!fclose(file));
    }
    reset(1);assert(d88_memory_run()==-603 && !owner && release_calls==1 && d88_memory_report[4]==4 && (d88_memory_report[9]>>16)==4);
    reset(2);assert(d88_memory_run()==-604 && !owner && release_calls==1);
    reset(3);assert(d88_memory_run()==-4 && !speed_calls && !allocate_calls && !probe_calls && !owner);
    assert(!heap_before.version && !allocation.version && !speed_results[0].result.version); /* no stale data */
    reset(4);assert(!d88_memory_run() && allocate_calls==2 && speed_calls==30 && !owner);
    assert(buffer_a==0x02550000u && buffer_b==0x02560000u);
    *(unsigned*)(size_t)0x02000204u=0;reset(0);assert(App_Main()==-1 && !shows && !speed_calls);
#ifdef _WIN32
    assert(VirtualFree(mapped,0,MEM_RELEASE));
#else
    assert(!munmap(mapped,0x00800000u));
#endif
    puts("memory frontend production model: PASS");return 0;
}
