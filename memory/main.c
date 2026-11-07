/* Foreground native benchmark. D88DEBUG may continue serving USB in task8.
 * Only memory_heap's owned allocation is written; all other regions are read.
 * No debugger protocol, clock, bus timing or original heap is reinitialized. */
#include "dsys.h"
#include "power_idle.h"
#include "memory_heap.h"
#include "memory_map.h"
#include "memory_speed.h"

typedef unsigned int u32;
typedef unsigned char u8;
#include "profiles.h"

#ifndef MEMORY_IMAGE_ID
#define MEMORY_IMAGE_ID 0u
#endif
#define REPORT_WORDS 2048u
#define REPORT_MAGIC 0x4d383844u
#define MAX_RESULTS 30u
#define WINDOW_BYTES 65536u

volatile unsigned d88_memory_report[REPORT_WORDS];
typedef struct speed_record {
    unsigned case_id,trial;
    d88_memory_speed_result result;
} speed_record;
static d88_mem_map_report map_report;
static d88_heap_snapshot heap_before,heap_after;
static d88_heap_probe heap_probe;
static d88_heap_allocation allocation;
static speed_record speed_results[MAX_RESULTS];
static unsigned results_used,buffer_a,buffer_b,cursor;
static unsigned rtc_start,rtc_end,ros_start,ros_end;

static void clear(void *memory,unsigned bytes)
{
    unsigned char *p=(unsigned char*)memory;
    while(bytes--) *p++=0;
}

static int profile(void)
{
    const probe_profile *p=profiles+1;
    unsigned i,b;
    for(i=0;i<6u;i++) if(*(volatile unsigned*)(0x02000200u+i*4u)!=p->tables[i]) return 0;
    for(i=0;i<3u;i++) for(b=0;b<40u;b++)
        if(*(volatile u8*)(p->signature_addresses[i]+b)!=p->signatures[i][b]) return 0;
    return 1;
}
static void show(const char *body,const char *title)
{
    tpDL_GUITable->MessageBox(HWND_DESKTOP,(const T_BYTE*)body,(const T_BYTE*)title,MB_OK);
}
static void add_section(unsigned type,const void *data,unsigned bytes,unsigned count)
{
    unsigned i,n=d88_memory_report[8],words=(bytes+3u)/4u;
    const unsigned char *src=(const unsigned char*)data;
    volatile unsigned char *dst=(volatile unsigned char*)(d88_memory_report+cursor);
    if(n>=8u || words>REPORT_WORDS-1u-cursor) return;
    d88_memory_report[16u+n*4u]=type;
    d88_memory_report[17u+n*4u]=cursor*4u;
    d88_memory_report[18u+n*4u]=bytes;
    d88_memory_report[19u+n*4u]=count;
    for(i=0;i<bytes;i++) dst[i]=src[i];
    cursor+=words;d88_memory_report[8]=n+1u;
}
static void seal(void)
{
    unsigned i,b,crc=0xffffffffu;
    const volatile unsigned char *data=(const volatile unsigned char*)d88_memory_report;
    for(i=0;i<(REPORT_WORDS-1u)*4u;i++) {
        crc^=data[i];
        for(b=0;b<8u;b++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    d88_memory_report[REPORT_WORDS-1u]=crc^0xffffffffu;
}
static int yield(void)
{
    int result=d88_power_idle_poll();
    if(result) return result;
    return tpDL_ROS33Table->ros33_dly_tsk(1);
}
static int prepare_copy(unsigned source,unsigned destination,unsigned seed)
{
    unsigned i,value;
    volatile unsigned *src=(volatile unsigned*)source,*dst=(volatile unsigned*)destination;
    if(!d88_memory_heap_owns(source,WINDOW_BYTES) || !d88_memory_heap_owns(destination,WINDOW_BYTES)) return -601;
    for(i=0;i<WINDOW_BYTES/4u;i++) {
        value=0x13579bdfu^(i*0x01010101u)^seed;
        src[i]=value;dst[i]=~value;
        if((i&2047u)==2047u && yield()!=0) return -602;
    }
    return 0;
}
static int verify_write(unsigned address)
{
    unsigned i;
    volatile unsigned *p=(volatile unsigned*)address;
    if(!d88_memory_heap_owns(address,WINDOW_BYTES)) return -601;
    for(i=0;i<WINDOW_BYTES/4u;i++) {
        if(p[i]!=D88_MEMORY_SPEED_WRITE_WORD) return -603;
        if((i&2047u)==2047u && yield()!=0) return -602;
    }
    return 0;
}
static int verify_copy(unsigned source,unsigned destination)
{
    unsigned i;
    volatile unsigned *src=(volatile unsigned*)source,*dst=(volatile unsigned*)destination;
    if(!d88_memory_heap_owns(source,WINDOW_BYTES) || !d88_memory_heap_owns(destination,WINDOW_BYTES)) return -601;
    for(i=0;i<WINDOW_BYTES/4u;i++) {
        if(src[i]!=dst[i]) return -604;
        if((i&2047u)==2047u && yield()!=0) return -602;
    }
    return 0;
}
static int speed_case(unsigned id,unsigned trial)
{
    static const unsigned reads[6]={0u,0x003c0000u,0x02010000u,0x02210000u,0x02410000u,0x02610000u};
    unsigned operation=1u,source=0,destination=0,bytes=16384u;
    int result;
    speed_record *record;
    if(id<6u) source=reads[id];
    else if(id<8u) {operation=2u;destination=id==6u?buffer_a:buffer_b;bytes=WINDOW_BYTES;}
    else {
        operation=3u;source=id==8u?buffer_a:buffer_b;destination=id==8u?buffer_b:buffer_a;bytes=WINDOW_BYTES;
        result=prepare_copy(source,destination,trial*16u+id);
        if(result) return result;
    }
    if(results_used>=MAX_RESULTS) return -605;
    record=speed_results+results_used++;record->case_id=id;record->trial=trial;
    d88_memory_report[7]=results_used;
    result=d88_memory_speed_run(operation,source,destination,bytes,d88_memory_heap_owns,&record->result);
    if(result) return result;
    if(operation==2u) result=verify_write(destination);
    else if(operation==3u) result=verify_copy(source,destination);
    if(!result) result=yield();
    return result;
}
int d88_memory_run(void)
{
    unsigned trial,k,id,i;
    int result=0,cleanup;
    results_used=buffer_a=buffer_b=0;
    rtc_start=rtc_end=ros_start=ros_end=0;
    clear(&map_report,sizeof(map_report));clear(&heap_before,sizeof(heap_before));
    clear(&heap_after,sizeof(heap_after));clear(&heap_probe,sizeof(heap_probe));
    clear(&allocation,sizeof(allocation));clear(speed_results,sizeof(speed_results));
    for(i=0;i<REPORT_WORDS;i++) d88_memory_report[i]=0;
    d88_memory_report[0]=REPORT_MAGIC;d88_memory_report[1]=1;
    d88_memory_report[2]=REPORT_WORDS*4u;d88_memory_report[3]=MEMORY_IMAGE_ID;
    d88_memory_report[4]=2;d88_memory_report[11]=(unsigned)d88_memory_report;
    result=d88_power_idle_begin();
    if(result) goto done;
    d88_memory_report[5]=1;
    result=d88_mem_map_capture(&map_report);
    if(result) goto done;
    d88_memory_report[9]|=1u;
    result=d88_memory_heap_snapshot(&heap_before);
    if(result) goto done;
    d88_memory_report[5]=2;
    result=d88_memory_heap_probe(&heap_probe);
    if(result) goto done;
    d88_memory_report[9]|=2u;
    d88_memory_report[5]=3;
    result=d88_memory_heap_allocate(0x00100000u,&allocation);
    if(result) result=d88_memory_heap_allocate(0x00040000u,&allocation);
    if(result) goto done;
    d88_memory_report[9]|=4u;
    buffer_a=(allocation.address+0xffffu)&~0xffffu;
    buffer_b=0x02600000u;
    if(buffer_b<buffer_a+WINDOW_BYTES || !d88_memory_heap_owns(buffer_b,WINDOW_BYTES)) buffer_b=buffer_a+WINDOW_BYTES;
    if(!d88_memory_heap_owns(buffer_a,WINDOW_BYTES) || !d88_memory_heap_owns(buffer_b,WINDOW_BYTES)) {result=-601;goto done;}
    result=d88_memory_speed_clock(&rtc_start,&ros_start);
    if(result) goto done;
    d88_memory_report[12]=ros_start;d88_memory_report[14]=rtc_start;
    d88_memory_report[5]=4;
    for(trial=0;trial<3u;trial++) for(k=0;k<10u;k++) {
        id=trial==1u?9u-k:k; /* Reverse middle trial to expose order effects. */
        result=speed_case(id,trial);
        if(result) goto done;
    }
    result=d88_memory_speed_clock(&rtc_end,&ros_end);
    if(result) goto done;
    d88_memory_report[13]=ros_end;d88_memory_report[15]=rtc_end;
    d88_memory_report[9]|=16u;
done:
    if(result) d88_memory_report[9]|=d88_memory_report[5]<<16;
    d88_memory_report[5]=5;
    cleanup=d88_memory_heap_release();
    if(cleanup && !result) result=cleanup;
    if(!cleanup) {d88_memory_report[9]|=8u;allocation.active=0;}
    cleanup=d88_memory_heap_snapshot(&heap_after);
    if(cleanup && !result) result=cleanup;
    d88_power_idle_end();
    d88_memory_report[6]=(unsigned)result;
    cursor=64u;
    add_section(1u,&map_report,sizeof(map_report),1u);
    add_section(2u,&heap_before,sizeof(heap_before),1u);
    add_section(3u,&heap_probe,sizeof(heap_probe),1u);
    add_section(4u,&allocation,sizeof(allocation),1u);
    add_section(5u,speed_results,results_used*sizeof(speed_record),results_used);
    add_section(6u,&heap_after,sizeof(heap_after),1u);
    d88_memory_report[10]=cursor*4u;
    d88_memory_report[4]=result?4u:3u;
    d88_memory_report[5]=0x100u;
    seal();
    return result;
}
T_WORD App_Main(void)
{
    int result;
    if(!profile()) return -1;
    show("Native memory test.\nLeave USB connected.\nPress OK to start.\nThe PC will read the result through D88DEBUG.","D88MEM START");
    result=d88_memory_run();
    show(result?"Test stopped with an error.\nLeave this report open for the PC.\nOwned memory was released unless its integrity guard failed.":
         "Memory test complete.\nLeave this report open until the PC saves it.",result?"D88MEM ERROR":"D88MEM DONE");
    return result;
}
