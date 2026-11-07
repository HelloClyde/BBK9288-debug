/* Offline captured-kernel GUI-context smoke. No payload or MMIO testing. */
#include "dsys.h"
#include "../memory/memory_heap.h"

typedef struct native_heap_smoke {
    unsigned magic,version,bytes,done,stage,result,psr,gp;
    unsigned context_before[4],context_after[4];
    unsigned results[6],owns[6],fault,canonical_restored;
    unsigned errno_before,errno_after_probe,errno_after;
    unsigned raw_before[5],raw_after_probe[5],raw_allocated[5],raw_after[5];
    d88_heap_snapshot before;
    d88_heap_probe probe;
    d88_heap_allocation allocation;
    d88_heap_snapshot allocated,after;
} native_heap_smoke;

volatile native_heap_smoke native_heap_smoke_report;
const unsigned native_heap_smoke_report_bytes=sizeof(native_heap_smoke);
typedef char smoke_words[(sizeof(native_heap_smoke)%4u==0)?1:-1];

static unsigned read32(unsigned address) {return *(volatile unsigned*)address;}
static void raw(unsigned *out)
{unsigned i;for(i=0;i<5u;i++) out[i]=read32(0x02069000u+i*4u);}
static void context(unsigned *out)
{
    out[0]=*(volatile unsigned char*)0x0206866cu;
    out[1]=*(volatile unsigned char*)0x0206866du;
    out[2]=read32(0x02068664u);
    out[3]=read32(0x022a9820u);
}
static int same(const unsigned *a,const unsigned *b,unsigned bytes)
{unsigned i;for(i=0;i<bytes/4u;i++) if(a[i]!=b[i]) return 0;return 1;}

T_WORD App_Main(void)
{
    native_heap_smoke *r=(native_heap_smoke*)&native_heap_smoke_report;
    int error;
    r->magic=0x3848534du;r->version=1;r->bytes=sizeof(*r);r->stage=1;
    __asm__ volatile("ld.w %0, %%psr":"=r"(r->psr)::"memory");
    __asm__ volatile("ld.w %0, %%r15":"=r"(r->gp)::"memory");
    context(r->context_before);raw(r->raw_before);r->errno_before=read32(0x020002b8u);
    error=d88_memory_heap_snapshot(&r->before);r->results[0]=(unsigned)error;
    if(error) goto finished;
    r->stage=2;
    error=d88_memory_heap_probe(&r->probe);r->results[1]=(unsigned)error;
    raw(r->raw_after_probe);r->errno_after_probe=read32(0x020002b8u);
    if(error) goto finished;
    r->stage=3;
    error=d88_memory_heap_allocate(1024u*1024u,&r->allocation);r->results[2]=(unsigned)error;
    if(error) goto finished;
    raw(r->raw_allocated);
    error=d88_memory_heap_snapshot(&r->allocated);r->results[3]=(unsigned)error;
    r->owns[0]=(unsigned)d88_memory_heap_owns(r->allocation.address,r->allocation.allocation_bytes);
    r->owns[1]=(unsigned)d88_memory_heap_owns(r->allocation.address+r->allocation.allocation_bytes-1u,1u);
    r->owns[2]=(unsigned)d88_memory_heap_owns(r->allocation.address-1u,1u);
    r->owns[3]=(unsigned)d88_memory_heap_owns(r->allocation.address+r->allocation.allocation_bytes,1u);
    r->owns[4]=(unsigned)d88_memory_heap_owns(r->allocation.address,~0u);
    r->stage=4;
    {int released=d88_memory_heap_release();r->results[4]=(unsigned)released;if(!error) error=released;}
    if(error) goto finished;
    r->owns[5]=(unsigned)d88_memory_heap_owns(r->allocation.address,1u);
    r->stage=5;
    error=d88_memory_heap_snapshot(&r->after);r->results[5]=(unsigned)error;
    raw(r->raw_after);
    if(!error) {
        r->canonical_restored=(unsigned)same((const unsigned*)&r->before,
                                            (const unsigned*)&r->after,sizeof(r->before));
        if(!r->canonical_restored) error=-901;
        else if(r->owns[0]!=1u || r->owns[1]!=1u || r->owns[2] || r->owns[3] ||
                r->owns[4] || r->owns[5]) error=-902;
    }
finished:
    r->fault=d88_memory_heap_fault();r->errno_after=read32(0x020002b8u);
    context(r->context_after);r->result=(unsigned)error;r->done=1;r->stage=6;
    return (T_WORD)error;
}
