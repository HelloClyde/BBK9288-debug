/* Production module tested against independently modeled directory mutation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define D88_MEMORY_HEAP_TEST 1
#include "../memory/memory_heap.c"
static const unsigned live_pairs[65][2]={
    {0x02500000u,0x00020001u},
    {0x02520000u,0x00001001u},
    {0x02521000u,0x00001001u},
    {0x02522000u,0x00004001u},
    {0x02526000u,0x00004001u},
    {0x0252a000u,0x00004001u},
    {0x0252e000u,0x00004001u},
    {0x02532000u,0x00000201u},
    {0x02532200u,0x00000201u},
    {0x02532400u,0x00000201u},
    {0x02532600u,0x00000201u},
    {0x02532800u,0x00000201u},
    {0x02532a00u,0x00000201u},
    {0x02532c00u,0x00000201u},
    {0x02532e00u,0x00000201u},
    {0x02533000u,0x00000201u},
    {0x02533200u,0x00000201u},
    {0x02533400u,0x00000201u},
    {0x02533600u,0x00000201u},
    {0x02533800u,0x00000201u},
    {0x02533a00u,0x00000201u},
    {0x02533c00u,0x00000201u},
    {0x02533e00u,0x00000201u},
    {0x02534000u,0x00000829u},
    {0x02534828u,0x00000181u},
    {0x025349a8u,0x00000169u},
    {0x02534b10u,0x00000655u},
    {0x02535164u,0x0000006du},
    {0x025351d0u,0x00000a01u},
    {0x02535bd0u,0x00000029u},
    {0x02535bf8u,0x00000029u},
    {0x02535c20u,0x00000029u},
    {0x02535c48u,0x00000029u},
    {0x02535c70u,0x00000029u},
    {0x02535c98u,0x00000029u},
    {0x02535cc0u,0x00000029u},
    {0x02535ce8u,0x00000029u},
    {0x02535d10u,0x00000029u},
    {0x02535d38u,0x00000029u},
    {0x02535d60u,0x00000029u},
    {0x02535d88u,0x00000029u},
    {0x02535db0u,0x000001c1u},
    {0x02535f70u,0x00000181u},
    {0x025360f0u,0x000000d5u},
    {0x025361c4u,0x00000029u},
    {0x025361ecu,0x00000101u},
    {0x025362ecu,0x00000009u},
    {0x025362f4u,0x00000901u},
    {0x02536bf4u,0x00000301u},
    {0x02536ef4u,0x00000011u},
    {0x02536f04u,0x00000039u},
    {0x02536f3cu,0x00000039u},
    {0x02536f74u,0x00000039u},
    {0x02536facu,0x00000039u},
    {0x02536fe4u,0x00000039u},
    {0x0253701cu,0x00000039u},
    {0x02537054u,0x00000039u},
    {0x0253708cu,0x00000039u},
    {0x025370c4u,0x00000039u},
    {0x025370fcu,0x00000039u},
    {0x02537134u,0x00000049u},
    {0x0253717cu,0x00000100u},
    {0x0253727cu,0x00019941u},
    {0x02550bbcu,0x0000037cu},
    {0x02550f38u,0x00000101u},
};
static unsigned state[5],records[512][2],native_errno,psr,gp,flags,depth;
static unsigned locked,locks,unlocks,allocations,frees,lock_error,unlock_error;
static unsigned root_error,native_mode,lock_race,read_race,code_bad_address;
static unsigned max_scan;
static unsigned char modified_code[1024];
static const struct {unsigned address,size;const unsigned char *signature;} spans[]={
 {MALLOC_ENTRY,sizeof(heap_malloc_signature),heap_malloc_signature},
 {FREE_ENTRY,sizeof(heap_free_signature),heap_free_signature},
 {INIT_ENTRY,sizeof(heap_init_signature),heap_init_signature},
 {0x021d3b08u,sizeof(heap_shift_down_signature),heap_shift_down_signature},
 {0x021d3b1au,sizeof(heap_shift_up_signature),heap_shift_up_signature},
 {LOCK_ENTRY,sizeof(heap_lock_signature),heap_lock_signature},
 {UNLOCK_ENTRY,sizeof(heap_unlock_signature),heap_unlock_signature}
};
unsigned d88_heap_test_read32(unsigned address)
{
    unsigned offset,slot,word;
    if(address==0x02000210u) return CRTL_TABLE+(root_error==1u?4u:0u);
    if(address==0x02000200u) return ROS_TABLE+(root_error==2u?4u:0u);
    if(address==CRTL_TABLE) return MALLOC_ENTRY+(root_error==3u?2u:0u);
    if(address==CRTL_TABLE+4u) return FREE_ENTRY+(root_error==4u?2u:0u);
    if(address==CRTL_TABLE+16u) return INIT_ENTRY+(root_error==5u?2u:0u);
    if(address==ROS_TABLE+23u*4u) return LOCK_ENTRY+(root_error==6u?2u:0u);
    if(address==ROS_TABLE+24u*4u) return UNLOCK_ENTRY+(root_error==7u?2u:0u);
    assert(locked && !(psr&16u));
    if(address==NATIVE_ERRNO) return native_errno;
    if(address>=HEAP_STATE && address<HEAP_STATE+20u) {
        offset=(address-HEAP_STATE)/4u;
        if(read_race && offset==4u) {read_race--;return state[offset]+1u;}
        return state[offset];
    }
    assert(address>=HEAP_END-8u*(D88_MEMORY_HEAP_MAX_RECORDS+2u) && address<HEAP_END);
    slot=(HEAP_END-1u-address)/8u;word=(address&4u)?1u:0u;
    assert(slot<512u);return records[slot][word];
}
unsigned char d88_heap_test_read8(unsigned address)
{
    unsigned i,offset=0;
    if(address==0x0206866cu) return (unsigned char)depth;
    if(address==0x0206866du) return (unsigned char)flags;
    for(i=0;i<sizeof(spans)/sizeof(spans[0]);i++) {
        if(address>=spans[i].address && address-spans[i].address<spans[i].size)
            return modified_code[offset+address-spans[i].address]^(address==code_bad_address);
        offset+=spans[i].size;
    }
    assert(0);return 0;
}
void d88_heap_test_write32(unsigned address,unsigned value)
{assert(locked && address==NATIVE_ERRNO);native_errno=value;}
unsigned d88_heap_test_psr(void) {return psr;}
unsigned d88_heap_test_gp(void) {return gp;}
int d88_heap_test_lock(void)
{
    locks++;assert(!locked && !depth && !(flags&7u) && (psr&0x710u)==0x10u);
    if(lock_error) return -1;
    locked=1;flags=3;psr&=~16u;
    if(lock_race==1u) root_error=1;
    if(lock_race==2u) code_bad_address=MALLOC_ENTRY+259u;
    if(lock_race==3u) flags=1;
    return 0;
}
int d88_heap_test_unlock(void)
{
    unlocks++;assert(locked);
    if(unlock_error) return -1;
    locked=0;flags=0;psr|=16u;return 0;
}
unsigned d88_heap_test_malloc(unsigned requested)
{
    unsigned i,j,n=(requested+3u)&~3u,gap=state[3]-state[2];
    assert(locked && flags==3u && !(psr&16u));allocations++;
    if(!requested) return 0;
    if(native_mode==1u || gap<8u) {native_errno=12;return 0;}
    if(native_mode==2u) return HEAP_END;
    if(native_mode==3u) return records[0][0];
    if(native_mode==6u && allocations==2u) n=(requested+2u)&~3u;
    for(i=0;i<state[4];i++) {
        unsigned p=records[i][0],size=records[i][1];
        if(i>max_scan) max_scan=i;
        if((size&1u) || size<n) continue;
        records[i][1]=n|1u;
        if(size>n) {
            for(j=state[4];j>i+1u;j--) {records[j][0]=records[j-1u][0];records[j][1]=records[j-1u][1];}
            records[i+1u][0]=p+n;records[i+1u][1]=size-n;
            state[4]++;state[3]-=8u;
        }
        return p;
    }
    if(gap<n+8u) {native_errno=12;return 0;}
    i=state[4]++;records[i][0]=state[2];records[i][1]=n|1u;
    state[2]+=n;state[3]-=8u;return records[i][0];
}
void d88_heap_test_free(unsigned pointer)
{
    unsigned i,j;
    assert(locked && flags==3u && !(psr&16u));frees++;
    if(native_mode==4u) return;
    for(i=0;i<state[4] && records[i][0]!=pointer;i++) {}
    assert(i<state[4] && (records[i][1]&1u));records[i][1]&=~1u;
    if(i && !(records[i-1u][1]&1u)) {
        records[i-1u][1]+=records[i][1];
        for(j=i;j+1u<state[4];j++) {records[j][0]=records[j+1u][0];records[j][1]=records[j+1u][1];}
        state[4]--;state[3]+=8u;i--;
    }
    if(i+1u<state[4] && !(records[i+1u][1]&1u)) {
        records[i][1]+=records[i+1u][1];
        for(j=i+1u;j+1u<state[4];j++) {records[j][0]=records[j+1u][0];records[j][1]=records[j+1u][1];}
        state[4]--;state[3]+=8u;
    }
    if(i+1u==state[4]) {state[2]=records[i][0];state[4]--;state[3]+=8u;}
    if(native_mode==5u) native_errno=99u;
}
static void setup(void)
{
    unsigned i,offset=0;
    state[0]=HEAP_START;state[1]=HEAP_END;state[2]=0x02551038u;state[3]=0x026ffdf0u;state[4]=65;
    memset(records,0,sizeof(records));memcpy(records,live_pairs,sizeof(live_pairs));
    for(i=0;i<sizeof(spans)/sizeof(spans[0]);i++) {memcpy(modified_code+offset,spans[i].signature,spans[i].size);offset+=spans[i].size;}
    native_errno=73u;psr=16u;gp=flags=depth=locked=locks=unlocks=allocations=frees=0;
    lock_error=unlock_error=root_error=native_mode=lock_race=read_race=code_bad_address=max_scan=0;
    owned_address=owned_bytes=heap_fault=0;
}
static void canonical(unsigned count,const unsigned *sizes,const unsigned *used)
{
    unsigned i,p=HEAP_START;state[4]=count;state[3]=HEAP_END-8u*(count+1u);
    for(i=0;i<count;i++) {records[i][0]=p;records[i][1]=sizes[i]|used[i];p+=sizes[i];}
    state[2]=p;
}
static void test_live(void)
{
    d88_heap_snapshot s;setup();assert(!d88_memory_heap_snapshot(&s));
    assert(s.version==1 && s.bytes==76 && s.record_count==65 && s.allocated_records==63 && s.free_records==2);
    assert(s.allocated_payload==330684 && s.free_holes==1148 && s.largest_hole==892);
    assert(s.tail_gap==1764792 && s.tail_capacity==1764784 && s.max_request==1764784);
    assert(s.free_capacity==1765932 && s.metadata_bytes==528 && s.directory_crc32==0xbc4b2063u);
    assert(!allocations && !frees && !locked && locks==1 && unlocks==1);
}
static void test_probe(void)
{
    d88_heap_probe p;setup();assert(!d88_memory_heap_probe(&p));
    assert(p.version==1 && p.bytes==204 && !p.result && p.candidate==1764784 && p.pointer==0x02551038u);
    assert(p.boundary_request==1764785 && !p.boundary_pointer && p.allocated_ok && p.boundary_failed);
    assert(p.state_restored && p.directory_restored && p.errno_before==73 && p.errno_after==73);
    assert(p.before.directory_crc32==p.after.directory_crc32 && allocations==2 && frees==1 && !locked);
    assert(!d88_memory_heap_fault());
    setup();native_mode=1;assert(d88_memory_heap_probe(&p)==D88_MEMORY_HEAP_E_ALLOC);
    assert(!frees && p.state_restored && p.directory_restored && native_errno==73 && !heap_fault);
    setup();native_mode=2;assert(d88_memory_heap_probe(&p)==D88_MEMORY_HEAP_E_NATIVE);
    assert(!frees && heap_fault && native_errno==73 && !locked);
    assert(d88_memory_heap_probe(&p)==D88_MEMORY_HEAP_E_FAULT && allocations==1);
    setup();native_mode=4;assert(d88_memory_heap_probe(&p)==D88_MEMORY_HEAP_E_RESTORE);
    assert(heap_fault && allocations==1 && frees==1 && !locked && native_errno==73);
    assert(d88_memory_heap_release()==D88_MEMORY_HEAP_E_FAULT);
    setup();native_mode=3;assert(d88_memory_heap_probe(&p)==D88_MEMORY_HEAP_E_NATIVE);
    assert(!frees && heap_fault);
    setup();native_mode=5;assert(!d88_memory_heap_probe(&p) && native_errno==73u);
    setup();native_mode=6;assert(d88_memory_heap_probe(&p)==D88_MEMORY_HEAP_E_BOUNDARY);
    assert(p.boundary_pointer && allocations==2 && frees==2 && heap_fault);
    assert(p.state_restored && p.directory_restored && native_errno==73u);
}
static void test_allocation(void)
{
    d88_heap_allocation a;d88_heap_snapshot before,after;unsigned p,gui;
    setup();assert(!d88_memory_heap_snapshot(&before));
    assert(!d88_memory_heap_allocate(1024u*1024u,&a));
    assert(a.address==0x02551038u && a.allocation_bytes==1048576u && a.active && !a.result);
    p=a.address;assert(d88_memory_heap_owns(p,1048576));
    assert(d88_memory_heap_owns(0x02600000u,65536u));
    assert(!d88_memory_heap_owns(p-4u,4u) && !d88_memory_heap_owns(p,1048577u));
    assert(!d88_memory_heap_owns(0xfffffffcu,8u) && !d88_memory_heap_owns(p,0u));
    assert(d88_memory_heap_allocate(4,&a)==D88_MEMORY_HEAP_E_BUSY);
    assert(!d88_memory_heap_release() && !d88_memory_heap_owns(p,4));
    assert(!d88_memory_heap_snapshot(&after));assert(before.directory_crc32==after.directory_crc32);
    assert(before.data_top==after.data_top && before.record_count==after.record_count);
    assert(!d88_memory_heap_release() && frees==1 && native_errno==73u);
    setup();assert(d88_memory_heap_allocate(1764784,&a)==D88_MEMORY_HEAP_E_RESERVE && !allocations);
    setup();assert(d88_memory_heap_allocate(0,&a)==D88_MEMORY_HEAP_E_ARGUMENT && !locks);
    assert(d88_memory_heap_allocate(0xffffffffu,&a)==D88_MEMORY_HEAP_E_ARGUMENT && !locks);
    setup();assert(!d88_memory_heap_allocate(1,&a) && a.allocation_bytes==4 && a.address==0x0253717cu);
    assert(!d88_memory_heap_release() && !heap_fault);
    setup();native_mode=1;assert(d88_memory_heap_allocate(1048576,&a)==D88_MEMORY_HEAP_E_ALLOC && !frees);
    setup();assert(!d88_memory_heap_allocate(1048576,&a));
    /* Legitimate GUI allocations between our operations must survive release. */
    assert(!d88_heap_test_lock());gui=d88_heap_test_malloc(256u);assert(gui);assert(!d88_heap_test_unlock());
    assert(!d88_memory_heap_release() && !heap_fault);
    assert(!d88_memory_heap_snapshot(&after));
    assert(after.allocated_payload==330684u+256u && used_record(&image_a,gui,256u));
    assert(!d88_heap_test_lock());d88_heap_test_free(gui);assert(!d88_heap_test_unlock());
    assert(!d88_memory_heap_snapshot(&after) && after.allocated_payload==330684u);
    {
        /* Aggregate free bytes alone cannot guarantee a128KiB GUI block. */
        unsigned sizes[11]={4u,262144u,4u,65536u,4u,65536u,4u,65536u,4u,65536u,0u};
        unsigned used[11]={1u,0u,1u,0u,1u,0u,1u,0u,1u,0u,1u},i,sum=0;
        setup();for(i=0;i<10u;i++) sum+=sizes[i];
        sizes[10]=2097152u-8u*12u-65544u-sum;
        canonical(11u,sizes,used);assert(!d88_memory_heap_snapshot(&before));
        assert(d88_memory_heap_allocate(262144u,&a)==D88_MEMORY_HEAP_E_RESERVE);
        assert(allocations==1u && frees==1u && !heap_fault && !owned_address);
        assert(!d88_memory_heap_snapshot(&after) && before.directory_crc32==after.directory_crc32);
    }
    setup();assert(!d88_memory_heap_allocate(1048576,&a));
    records[65][1]+=4u;state[2]+=4u; /* Foreign resize of our record. */
    assert(d88_memory_heap_release()==D88_MEMORY_HEAP_E_OWNER && !frees && heap_fault);
}
static void test_layout(void)
{
    unsigned i,old,sizes[3],used[3]={1,0,1};d88_heap_snapshot s;d88_heap_probe p;
    for(i=0;i<8u;i++) {
        setup();
        if(i==0) state[0]+=4u;
        else if(i==1) state[1]-=4u;
        else if(i==2) state[2]|=1u;
        else if(i==3) state[3]-=8u;
        else if(i==4) records[1][0]+=4u;
        else if(i==5) records[0][1]|=2u;
        else if(i==6) records[64][1]&=~1u;
        else records[0][1]=0xfffffffdu;
        assert(d88_memory_heap_snapshot(&s)==D88_MEMORY_HEAP_E_LAYOUT && !allocations && !locked);
    }
    setup();state[4]=D88_MEMORY_HEAP_MAX_RECORDS+1u;
    assert(d88_memory_heap_snapshot(&s)==D88_MEMORY_HEAP_E_LIMIT && !allocations);
    setup();read_race=1;assert(d88_memory_heap_snapshot(&s)==D88_MEMORY_HEAP_E_LAYOUT);
    setup();canonical(0,0,0);assert(!d88_memory_heap_snapshot(&s));
    assert(s.max_request==2097136u && s.metadata_bytes==8u);
    sizes[0]=4;sizes[1]=4;sizes[2]=2097152u-32u-8u;
    setup();canonical(3,sizes,used);assert(!d88_memory_heap_snapshot(&s));
    assert(s.free_holes==4 && !s.tail_gap && !s.max_request && !s.free_capacity);
    assert(d88_memory_heap_probe(&p)==D88_MEMORY_HEAP_E_ALLOC && !allocations);
    setup();old=records[61][1];records[61][1]|=1u;records[63][1]|=1u;
    assert(!d88_memory_heap_snapshot(&s) && !s.free_holes);records[61][1]=old;
}
static void test_guard(void)
{
    unsigned i,j;d88_heap_snapshot s;
    for(i=1;i<=7u;i++) {setup();root_error=i;assert(d88_memory_heap_snapshot(&s)==D88_MEMORY_HEAP_E_PROFILE && !locks && !allocations);}
    for(i=0;i<sizeof(spans)/sizeof(spans[0]);i++) for(j=0;j<spans[i].size;j++) {
        setup();code_bad_address=spans[i].address+j;
        assert(d88_memory_heap_snapshot(&s)==D88_MEMORY_HEAP_E_PROFILE && !locks && !allocations);
    }
    for(i=0;i<7u;i++) {
        setup();if(i==0) psr=0;else if(i==1) psr=0x110;
        else if(i==2) gp=4;else if(i==3) depth=1;else flags=1u<<(i-4u);
        assert(d88_memory_heap_snapshot(&s)==D88_MEMORY_HEAP_E_CONTEXT && !locks);
    }
    for(i=1;i<=3u;i++) {
        setup();lock_race=i;
        assert(d88_memory_heap_snapshot(&s)==(i==3?D88_MEMORY_HEAP_E_CONTEXT:D88_MEMORY_HEAP_E_PROFILE));
        assert(locks==1 && unlocks==1 && !locked && !allocations);
    }
    setup();lock_error=1;assert(d88_memory_heap_snapshot(&s)==D88_MEMORY_HEAP_E_LOCK && !unlocks);
    setup();unlock_error=1;assert(d88_memory_heap_snapshot(&s)==D88_MEMORY_HEAP_E_UNLOCK && heap_fault);
}
int main(int argc,char **argv)
{
    unsigned which=argc>1?(unsigned)atoi(argv[1]):0;
    if(which==0) test_live();
    else if(which==1) test_probe();
    else if(which==2) test_allocation();
    else if(which==3) test_layout();
    else if(which==4) test_guard();
    else return 2;
    puts("memory heap model: PASS");return 0;
}
