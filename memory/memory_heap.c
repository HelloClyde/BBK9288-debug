/* Native captured allocator only; no SDK table-tail assumptions. */
#include "memory_heap.h"
typedef char heap_unsigned_is_u32[(sizeof(unsigned)==4u)?1:-1];
typedef char heap_snapshot_size[(sizeof(d88_heap_snapshot)==76u)?1:-1];
typedef char heap_probe_size[(sizeof(d88_heap_probe)==204u)?1:-1];
typedef char heap_allocation_size[(sizeof(d88_heap_allocation)==24u)?1:-1];
#define HEAP_STATE 0x02069000u
#define HEAP_START 0x02500000u
#define HEAP_END 0x02700000u
#define NATIVE_ERRNO 0x020002b8u
#define CRTL_TABLE 0x0200021cu
#define ROS_TABLE 0x021e81e8u
#define MALLOC_ENTRY 0x021d330cu
#define FREE_ENTRY 0x021d353au
#define INIT_ENTRY 0x021d3410u
#define LOCK_ENTRY 0x021ce8aeu
#define UNLOCK_ENTRY 0x021ce8d8u
#ifdef D88_MEMORY_HEAP_TEST
extern unsigned d88_heap_test_read32(unsigned);
extern unsigned char d88_heap_test_read8(unsigned);
extern void d88_heap_test_write32(unsigned,unsigned);
extern unsigned d88_heap_test_psr(void);
extern unsigned d88_heap_test_gp(void);
extern int d88_heap_test_lock(void);
extern int d88_heap_test_unlock(void);
extern unsigned d88_heap_test_malloc(unsigned);
extern void d88_heap_test_free(unsigned);
#define R32(a) d88_heap_test_read32(a)
#define R8(a) d88_heap_test_read8(a)
#define W32(a,v) d88_heap_test_write32(a,v)
#define PSR() d88_heap_test_psr()
#define GP() d88_heap_test_gp()
#define LOCK() d88_heap_test_lock()
#define UNLOCK() d88_heap_test_unlock()
#define ALLOC(n) d88_heap_test_malloc(n)
#define FREE(p) d88_heap_test_free(p)
#else
#define R32(a) (*(volatile unsigned*)(a))
#define R8(a) (*(volatile unsigned char*)(a))
#define W32(a,v) (*(volatile unsigned*)(a)=(v))
#define LOCK() ((int(*)(void))LOCK_ENTRY)()
#define UNLOCK() ((int(*)(void))UNLOCK_ENTRY)()
#define ALLOC(n) ((unsigned)((void*(*)(unsigned))MALLOC_ENTRY)(n))
#define FREE(p) ((void(*)(void*))FREE_ENTRY)((void*)(p))
static unsigned current_psr(void) {unsigned n;__asm__ volatile("ld.w %0, %%psr":"=r"(n)::"memory");return n;}
static unsigned current_gp(void) {unsigned n;__asm__ volatile("ld.w %0, %%r15":"=r"(n)::"memory");return n;}
#define PSR() current_psr()
#define GP() current_gp()
#endif
static const unsigned char heap_malloc_signature[260]={
    0x03,0x02,0x06,0x68,0x7e,0x19,0x04,0x6c,0x34,0xd0,0x04,0xd0,0xf4,0x30,0x41,0x2e,
    0x34,0xd0,0x0c,0xd0,0xf4,0x30,0x34,0xd0,0x08,0xd0,0xf5,0x30,0x54,0x26,0x74,0x68,
    0x6b,0x0f,0x81,0x64,0x08,0x6c,0x34,0xd0,0x10,0xd0,0xf4,0x30,0x48,0x2a,0x3b,0x13,
    0x47,0x2e,0x04,0xc0,0x15,0x30,0x54,0x2e,0x14,0x70,0x2e,0x1b,0x64,0x2e,0x34,0x60,
    0x40,0x2e,0xc0,0x73,0x05,0x2a,0x28,0x15,0x53,0x2e,0x03,0x26,0x04,0x2e,0x14,0x74,
    0x04,0xc0,0x14,0x3c,0x12,0x30,0x03,0x68,0x4d,0x19,0x87,0x26,0x34,0xd0,0x0c,0xd0,
    0xf6,0x30,0x00,0xc0,0x03,0xc0,0xcb,0x1d,0x17,0x64,0x81,0x64,0x04,0xc0,0x13,0x3c,
    0x24,0x2e,0x04,0x22,0x14,0x3c,0x34,0xd0,0x0c,0xd0,0xf4,0x30,0x84,0x64,0x34,0xd0,
    0x0c,0xd0,0xf4,0x3c,0x34,0xd0,0x10,0xd0,0xf4,0x30,0x14,0x60,0x34,0xd0,0x10,0xd0,
    0xf4,0x3c,0x37,0x1f,0x24,0x2e,0x18,0x60,0x34,0xd0,0x10,0xd0,0xf7,0x30,0x78,0x2a,
    0xc9,0x15,0x81,0x64,0x34,0xd0,0x0c,0xd0,0xf8,0x30,0x34,0xd0,0x08,0xd0,0xf7,0x30,
    0x84,0x2e,0x74,0x26,0x65,0x2e,0x35,0x60,0x56,0x2e,0xc6,0x73,0x65,0x2e,0x85,0x60,
    0x54,0x2a,0x1a,0x15,0x72,0x2e,0x12,0x3c,0x64,0x2e,0x14,0x74,0x04,0xc0,0x14,0x3c,
    0x64,0x2e,0x24,0x22,0x34,0xd0,0x08,0xd0,0xf4,0x3c,0x34,0xd0,0x10,0xd0,0xf4,0x30,
    0x14,0x60,0x34,0xd0,0x10,0xd0,0xf4,0x3c,0x84,0x2e,0x84,0x64,0x34,0xd0,0x0c,0xd0,
    0xf4,0x3c,0x07,0x1f,0x24,0x2e,0xc4,0x6c,0x00,0xd0,0xb8,0xc2,0xf4,0x3c,0x04,0x6c,
    0x43,0x02,0x40,0x06,
};
static const unsigned char heap_free_signature[330]={
    0x02,0x02,0x06,0x68,0x00,0xc0,0xa0,0x19,0x02,0x6c,0x34,0xd0,0x04,0xd0,0xf4,0x30,
    0x40,0x2e,0x80,0x64,0x34,0xd0,0x10,0xd0,0xf4,0x30,0x42,0x2a,0x00,0xc0,0x94,0x13,
    0x45,0x2e,0x04,0x30,0x46,0x2a,0x00,0xc0,0x87,0x1b,0x58,0x2e,0x04,0xc0,0x04,0x30,
    0x41,0x2e,0xe1,0x73,0x04,0xc0,0x01,0x3c,0x84,0x2e,0x14,0x64,0x42,0x2a,0x31,0x1b,
    0x02,0x68,0x34,0xd0,0x10,0xd0,0xf2,0x3c,0x34,0xd0,0x0c,0xd0,0xf4,0x30,0x47,0x2e,
    0x87,0x60,0x34,0xd0,0x0c,0xd0,0xf7,0x3c,0x34,0xd0,0x08,0xd0,0xf4,0x30,0x46,0x2e,
    0x16,0x26,0x34,0xd0,0x08,0xd0,0xf6,0x3c,0x02,0x68,0x6e,0x19,0x80,0x60,0x04,0xc0,
    0x05,0x30,0x54,0x2e,0x14,0x70,0x68,0x1b,0x14,0x2e,0x54,0x22,0x04,0xc0,0x04,0x3c,
    0x84,0x2e,0x24,0x64,0x34,0xd0,0x10,0xd0,0xf4,0x3c,0x74,0x2e,0x84,0x60,0x34,0xd0,
    0x0c,0xd0,0xf4,0x3c,0x64,0x2e,0x54,0x26,0x34,0xd0,0x08,0xd0,0xf4,0x3c,0x54,0x1e,
    0x24,0x18,0x0c,0xc0,0x04,0x30,0x14,0x70,0x20,0x1b,0x06,0x2e,0x80,0x60,0x04,0xc0,
    0x04,0x30,0x41,0x22,0x04,0xc0,0x01,0x3c,0x34,0xd0,0x10,0xd0,0xf4,0x30,0x24,0x26,
    0x47,0x2e,0x00,0xc0,0x02,0xc0,0x8d,0x1d,0x17,0x64,0x34,0xd0,0x10,0xd0,0xf4,0x30,
    0x14,0x64,0x34,0xd0,0x10,0xd0,0xf4,0x3c,0x34,0xd0,0x0c,0xd0,0xf4,0x30,0x84,0x60,
    0x34,0xd0,0x0c,0xd0,0xf4,0x3c,0x12,0x64,0x34,0xd0,0x10,0xd0,0xf7,0x30,0x72,0x2a,
    0x2b,0x13,0x06,0x2e,0x86,0x64,0x04,0xc0,0x65,0x30,0x54,0x2e,0x14,0x70,0x24,0x1b,
    0x14,0x2e,0x54,0x22,0x04,0xc0,0x04,0x3c,0x12,0x60,0x74,0x2e,0x24,0x26,0x47,0x2e,
    0x00,0xc0,0x02,0xc0,0x66,0x1d,0x17,0x64,0x34,0xd0,0x10,0xd0,0xf4,0x30,0x14,0x64,
    0x34,0xd0,0x10,0xd0,0xf4,0x3c,0x34,0xd0,0x0c,0xd0,0xf4,0x30,0x84,0x60,0x34,0xd0,
    0x0c,0xd0,0xf4,0x3c,0x09,0x1e,0x12,0x60,0x34,0xd0,0x10,0xd0,0xf5,0x30,0x52,0x2a,
    0xff,0xdf,0x70,0x15,0x80,0x64,0x42,0x02,0x40,0x06,
};
static const unsigned char heap_init_signature[58]={
    0x36,0x60,0xc6,0x73,0x34,0xd0,0x00,0xd0,0xf6,0x3c,0x37,0x60,0xc7,0x73,0x34,0xd0,
    0x04,0xd0,0xf7,0x3c,0x64,0x2e,0x04,0x61,0x74,0x2a,0x0e,0x11,0x74,0x2e,0x34,0xd0,
    0x08,0xd0,0xf6,0x3c,0x84,0x64,0x34,0xd0,0x0c,0xd0,0xf4,0x3c,0x04,0x6c,0x34,0xd0,
    0x10,0xd0,0xf4,0x3c,0x02,0x1e,0x14,0x6c,0x40,0x06,
};
static const unsigned char heap_shift_down_signature[18]={
    0x68,0x2e,0x88,0x60,0x84,0x31,0x64,0x3d,0x84,0x31,0x64,0x3d,0x17,0x64,0xfb,0x1a,
    0x40,0x06,
};
static const unsigned char heap_shift_up_signature[28]={
    0x46,0x60,0x68,0x2e,0x88,0x64,0x84,0x30,0x64,0x3c,0x46,0x64,0x48,0x64,0x84,0x30,
    0x64,0x3c,0x46,0x64,0x17,0x64,0xf8,0x1b,0x48,0x64,0x40,0x06,
};
static const unsigned char heap_lock_signature[42]={
    0x34,0xd0,0x6c,0xc6,0xf4,0x24,0x04,0x68,0x00,0xc0,0x0d,0x1a,0x34,0xd0,0x6d,0xc6,
    0xf4,0x24,0x34,0x74,0x34,0xd0,0x6d,0xc6,0xf4,0x34,0x04,0xa4,0xf4,0x72,0x40,0xa0,
    0x04,0x1f,0x04,0x6c,0xfe,0xdf,0xb4,0x6f,0x40,0x06,
};
static const unsigned char heap_unlock_signature[58]={
    0x34,0xd0,0x6c,0xc6,0xf4,0x24,0x04,0x68,0x05,0x18,0xfe,0xdf,0xb4,0x6f,0x00,0xc0,
    0x14,0x1e,0x34,0xd0,0x6d,0xc6,0xf4,0x24,0x03,0xc0,0xc4,0x73,0x34,0xd0,0x6d,0xc6,
    0xf4,0x34,0x34,0xd0,0x6e,0xc6,0xf0,0xa8,0x04,0x18,0x00,0xc0,0x13,0xc0,0x38,0x1c,
    0x04,0xa4,0x04,0x75,0x40,0xa0,0x04,0x6c,0x40,0x06,
};
typedef struct heap_image {
    unsigned state[5];
    unsigned pair[D88_MEMORY_HEAP_MAX_RECORDS][2];
} heap_image;
static heap_image image_a,image_b;
static unsigned owned_address,owned_bytes,heap_fault;

static void clear_words(void *p,unsigned bytes)
{unsigned i;unsigned *w=(unsigned*)p;for(i=0;i<bytes/4u;i++) w[i]=0;}
static int code_is(unsigned address,const unsigned char *signature,unsigned bytes)
{unsigned i;for(i=0;i<bytes;i++) if(R8(address+i)!=signature[i]) return 0;return 1;}
static int profile(void)
{
    if(GP()!=0u || R32(0x02000210u)!=CRTL_TABLE ||
       R32(0x02000200u)!=ROS_TABLE ||
       R32(CRTL_TABLE)!=MALLOC_ENTRY || R32(CRTL_TABLE+4u)!=FREE_ENTRY ||
       R32(CRTL_TABLE+16u)!=INIT_ENTRY ||
       R32(ROS_TABLE+23u*4u)!=LOCK_ENTRY || R32(ROS_TABLE+24u*4u)!=UNLOCK_ENTRY)
        return D88_MEMORY_HEAP_E_PROFILE;
    if(!code_is(MALLOC_ENTRY,heap_malloc_signature,sizeof(heap_malloc_signature)) ||
       !code_is(FREE_ENTRY,heap_free_signature,sizeof(heap_free_signature)) ||
       !code_is(INIT_ENTRY,heap_init_signature,sizeof(heap_init_signature)) ||
       !code_is(0x021d3b08u,heap_shift_down_signature,sizeof(heap_shift_down_signature)) ||
       !code_is(0x021d3b1au,heap_shift_up_signature,sizeof(heap_shift_up_signature)) ||
       !code_is(LOCK_ENTRY,heap_lock_signature,sizeof(heap_lock_signature)) ||
       !code_is(UNLOCK_ENTRY,heap_unlock_signature,sizeof(heap_unlock_signature)))
        return D88_MEMORY_HEAP_E_PROFILE;
    return 0;
}
static int enter(void)
{
    int result;
    if((PSR()&0x710u)!=0x10u || R8(0x0206866cu) || (R8(0x0206866du)&7u) || GP())
        return D88_MEMORY_HEAP_E_CONTEXT;
    result=profile();if(result) return result;
    if(LOCK()!=0) return D88_MEMORY_HEAP_E_LOCK;
    result=profile();
    if(!result && ((PSR()&0x710u)!=0u || R8(0x0206866cu) ||
                  (R8(0x0206866du)&7u)!=3u)) result=D88_MEMORY_HEAP_E_CONTEXT;
    if(result) {
        if(UNLOCK()!=0) {heap_fault=1;return D88_MEMORY_HEAP_E_UNLOCK;}
        return result;
    }
    return 0;
}
static int leave(int result)
{
    if(UNLOCK()!=0) {heap_fault=1;return D88_MEMORY_HEAP_E_UNLOCK;}
    return result;
}
static int integrity_error(int result)
{heap_fault=1;return result;}
static int take(heap_image *image)
{
    unsigned i,count,top,bottom,expected,previous_free=0;
    for(i=0;i<5u;i++) image->state[i]=R32(HEAP_STATE+i*4u);
    count=image->state[4];top=image->state[2];bottom=image->state[3];
    if(count>D88_MEMORY_HEAP_MAX_RECORDS) return D88_MEMORY_HEAP_E_LIMIT;
    if(image->state[0]!=HEAP_START || image->state[1]!=HEAP_END ||
       ((top|bottom)&3u) || bottom!=HEAP_END-8u*(count+1u) ||
       top<HEAP_START || top>bottom) return D88_MEMORY_HEAP_E_LAYOUT;
    expected=HEAP_START;
    for(i=0;i<count;i++) {
        unsigned slot=HEAP_END-8u*(i+1u),pointer=R32(slot),flags=R32(slot+4u);
        unsigned size=flags&~1u,free_record=!(flags&1u);
        if((flags&2u) || !size || pointer!=expected || size>top-expected ||
           (free_record && (previous_free || i+1u==count)))
            return D88_MEMORY_HEAP_E_LAYOUT;
        image->pair[i][0]=pointer;image->pair[i][1]=flags;
        expected+=size;previous_free=free_record;
    }
    if(expected!=top) return D88_MEMORY_HEAP_E_LAYOUT;
    for(i=0;i<5u;i++) if(R32(HEAP_STATE+i*4u)!=image->state[i])
        return D88_MEMORY_HEAP_E_LAYOUT;
    return 0;
}
static unsigned directory_crc(const heap_image *image)
{
    unsigned crc=~0u,i,j,k,v;
    /* Byte order is the raw ascending-address [end-count*8,end) READ. */
    for(i=image->state[4];i;i--) for(j=0;j<2u;j++) {
        v=image->pair[i-1u][j];
        for(k=0;k<4u;k++) {
            unsigned bit;crc^=v&255u;v>>=8;
            for(bit=0;bit<8u;bit++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
        }
    }
    return ~crc;
}
static void describe(const heap_image *image,d88_heap_snapshot *out)
{
    unsigned i,count=image->state[4],gap;
    clear_words(out,sizeof(*out));out->version=D88_MEMORY_HEAP_VERSION;out->bytes=sizeof(*out);
    out->start=image->state[0];out->end=image->state[1];out->data_top=image->state[2];
    out->directory_bottom=image->state[3];out->record_count=count;
    for(i=0;i<count;i++) {
        unsigned flags=image->pair[i][1],size=flags&~1u;
        if(flags&1u) {out->allocated_records++;out->allocated_payload+=size;}
        else {out->free_records++;out->free_holes+=size;if(size>out->largest_hole) out->largest_hole=size;}
    }
    gap=out->directory_bottom-out->data_top;out->tail_gap=gap;
    out->tail_capacity=gap>=8u?(gap-8u)&~3u:0u;
    if(gap>=8u) {
        out->max_request=out->largest_hole>out->tail_capacity?out->largest_hole:out->tail_capacity;
        out->free_capacity=out->free_holes+out->tail_capacity;
    }
    out->metadata_bytes=8u*(count+1u);out->directory_crc32=directory_crc(image);
}
static int same_state(const heap_image *a,const heap_image *b)
{unsigned i;for(i=0;i<5u;i++) if(a->state[i]!=b->state[i]) return 0;return 1;}
static int same_directory(const heap_image *a,const heap_image *b)
{
    unsigned i;if(a->state[4]!=b->state[4]) return 0;
    for(i=0;i<a->state[4];i++) if(a->pair[i][0]!=b->pair[i][0] || a->pair[i][1]!=b->pair[i][1]) return 0;
    return 1;
}
static int used_record(const heap_image *image,unsigned pointer,unsigned bytes)
{
    unsigned i;for(i=0;i<image->state[4];i++)
        if(image->pair[i][0]==pointer && image->pair[i][1]==(bytes|1u)) return 1;
    return 0;
}
static int allocated_from_free(const heap_image *before,const heap_image *after,unsigned pointer,unsigned bytes)
{
    unsigned i,available=0;
    if(!pointer || (pointer&3u) || pointer<HEAP_START || bytes>HEAP_END-pointer ||
       pointer>=HEAP_END || !used_record(after,pointer,bytes)) return 0;
    if(pointer==before->state[2]) available=1;
    for(i=0;i<before->state[4];i++) {
        unsigned p=before->pair[i][0],flags=before->pair[i][1];
        if(!(flags&1u) && pointer==p && bytes<=flags) available=1;
        if((flags&1u) && !used_record(after,p,flags&~1u)) return 0;
    }
    return (int)available;
}
static int release_preserved(const heap_image *before,const heap_image *after,unsigned pointer,unsigned bytes)
{
    unsigned i,before_used=0,after_used=0;
    for(i=0;i<before->state[4];i++) if(before->pair[i][1]&1u) {
        unsigned p=before->pair[i][0],n=before->pair[i][1]&~1u;before_used+=n;
        if(p!=pointer && !used_record(after,p,n)) return 0;
    }
    for(i=0;i<after->state[4];i++) if(after->pair[i][1]&1u) after_used+=after->pair[i][1]&~1u;
    return before_used>=bytes && after_used==before_used-bytes && !used_record(after,pointer,bytes);
}
int d88_memory_heap_snapshot(d88_heap_snapshot *out)
{
    int result;
    if(!out) return D88_MEMORY_HEAP_E_ARGUMENT;
    clear_words(out,sizeof(*out));out->version=D88_MEMORY_HEAP_VERSION;out->bytes=sizeof(*out);
    result=enter();if(!result) {result=take(&image_a);if(!result) describe(&image_a,out);result=leave(result);}
    out->result=(unsigned)result;return result;
}
static int probe_one(unsigned request,unsigned bytes,unsigned *pointer)
{
    int result;unsigned i,actual_bytes=0;
    *pointer=ALLOC(request);
    result=take(&image_b);if(result) return integrity_error(result);
    if(!*pointer) {
        if(!same_state(&image_a,&image_b) || !same_directory(&image_a,&image_b))
            return integrity_error(D88_MEMORY_HEAP_E_RESTORE);
        return 0;
    }
    for(i=0;i<image_b.state[4];i++)
        if(image_b.pair[i][0]==*pointer && (image_b.pair[i][1]&1u))
            actual_bytes=image_b.pair[i][1]&~1u;
    /* An unexpected success can still be reclaimed if a complete canonical
     * directory proves this is a new owned block. Never free an arbitrary
     * returned address, even when the allocator's result is an error. */
    if(!actual_bytes || !allocated_from_free(&image_a,&image_b,*pointer,actual_bytes))
        return integrity_error(D88_MEMORY_HEAP_E_NATIVE);
    if(profile()!=0) return integrity_error(D88_MEMORY_HEAP_E_PROFILE);
    FREE(*pointer);
    result=take(&image_b);if(result) return integrity_error(result);
    if(!same_state(&image_a,&image_b) || !same_directory(&image_a,&image_b))
        return integrity_error(D88_MEMORY_HEAP_E_RESTORE);
    if(actual_bytes!=bytes)
        return integrity_error((request&3u)?D88_MEMORY_HEAP_E_BOUNDARY:D88_MEMORY_HEAP_E_NATIVE);
    return 0;
}
int d88_memory_heap_probe(d88_heap_probe *out)
{
    int result;unsigned saved_errno=0,have_errno=0;
    if(!out) return D88_MEMORY_HEAP_E_ARGUMENT;
    clear_words(out,sizeof(*out));out->version=D88_MEMORY_HEAP_VERSION;out->bytes=sizeof(*out);
    if(heap_fault) result=D88_MEMORY_HEAP_E_FAULT;
    else if(owned_address) result=D88_MEMORY_HEAP_E_BUSY;
    else result=enter();
    if(result) {out->result=(unsigned)result;return result;}
    result=take(&image_a);
    if(!result && image_a.state[4]>=D88_MEMORY_HEAP_MAX_RECORDS) result=D88_MEMORY_HEAP_E_LIMIT;
    if(!result) {
        describe(&image_a,&out->before);out->candidate=out->before.max_request;
        if(!out->candidate) result=D88_MEMORY_HEAP_E_ALLOC;
    }
    if(!result) {
        saved_errno=R32(NATIVE_ERRNO);out->errno_before=saved_errno;have_errno=1;
        result=probe_one(out->candidate,out->candidate,&out->pointer);
        if(!result && !out->pointer) result=D88_MEMORY_HEAP_E_ALLOC;
        if(!result) {
            out->allocated_ok=1;out->boundary_request=out->candidate+1u;
            result=probe_one(out->boundary_request,out->candidate+4u,&out->boundary_pointer);
            if(!result && out->boundary_pointer) result=integrity_error(D88_MEMORY_HEAP_E_BOUNDARY);
            if(!result) out->boundary_failed=1;
        }
        {
            int final=take(&image_b);
            if(final) result=integrity_error(final);
            else {
                describe(&image_b,&out->after);
                out->state_restored=(unsigned)same_state(&image_a,&image_b);
                out->directory_restored=(unsigned)same_directory(&image_a,&image_b);
                if(!out->state_restored || !out->directory_restored) result=integrity_error(D88_MEMORY_HEAP_E_RESTORE);
            }
        }
    }
    if(have_errno) {W32(NATIVE_ERRNO,saved_errno);out->errno_after=R32(NATIVE_ERRNO);if(out->errno_after!=saved_errno) result=integrity_error(D88_MEMORY_HEAP_E_RESTORE);}
    result=leave(result);out->result=(unsigned)result;return result;
}
int d88_memory_heap_allocate(unsigned requested,d88_heap_allocation *out)
{
    int result;unsigned rounded,pointer=0,saved_errno;
    d88_heap_snapshot before,after;
    if(!out) return D88_MEMORY_HEAP_E_ARGUMENT;
    clear_words(out,sizeof(*out));out->version=D88_MEMORY_HEAP_VERSION;out->bytes=sizeof(*out);
    if(!requested || requested>HEAP_END-HEAP_START) result=D88_MEMORY_HEAP_E_ARGUMENT;
    else if(heap_fault) result=D88_MEMORY_HEAP_E_FAULT;
    else if(owned_address) result=D88_MEMORY_HEAP_E_BUSY;
    else result=enter();
    if(result) {out->result=(unsigned)result;return result;}
    rounded=(requested+3u)&~3u;saved_errno=R32(NATIVE_ERRNO);
    result=take(&image_a);
    if(!result && image_a.state[4]>=D88_MEMORY_HEAP_MAX_RECORDS) result=D88_MEMORY_HEAP_E_LIMIT;
    if(!result) {
        describe(&image_a,&before);
        if(before.free_capacity<rounded+8u+D88_MEMORY_HEAP_GUI_RESERVE || before.max_request<rounded)
            result=D88_MEMORY_HEAP_E_RESERVE;
    }
    if(!result) {
        pointer=ALLOC(requested);result=take(&image_b);
        if(result) result=integrity_error(result);
        else if(!pointer) {
            if(!same_state(&image_a,&image_b) || !same_directory(&image_a,&image_b)) result=integrity_error(D88_MEMORY_HEAP_E_RESTORE);
            else result=D88_MEMORY_HEAP_E_ALLOC;
        } else if(!allocated_from_free(&image_a,&image_b,pointer,rounded)) result=integrity_error(D88_MEMORY_HEAP_E_NATIVE);
        else {
            describe(&image_b,&after);
            owned_address=pointer;owned_bytes=rounded;
            if(after.free_capacity<D88_MEMORY_HEAP_GUI_RESERVE || after.max_request<D88_MEMORY_HEAP_GUI_RESERVE) {
                if(profile()!=0) result=integrity_error(D88_MEMORY_HEAP_E_PROFILE);
                else {
                    FREE(pointer);result=take(&image_b);
                    if(result || !same_state(&image_a,&image_b) || !same_directory(&image_a,&image_b))
                        result=integrity_error(D88_MEMORY_HEAP_E_RESTORE);
                    else {owned_address=owned_bytes=0;result=D88_MEMORY_HEAP_E_RESERVE;}
                }
            }
        }
    }
    W32(NATIVE_ERRNO,saved_errno);
    if(R32(NATIVE_ERRNO)!=saved_errno) result=integrity_error(D88_MEMORY_HEAP_E_RESTORE);
    result=leave(result);
    if(!result) {out->address=owned_address;out->allocation_bytes=owned_bytes;out->active=1;}
    out->result=(unsigned)result;return result;
}
int d88_memory_heap_release(void)
{
    int result;unsigned saved_errno;
    if(heap_fault) return D88_MEMORY_HEAP_E_FAULT;
    if(!owned_address) return 0;
    result=enter();if(result) return result;
    saved_errno=R32(NATIVE_ERRNO);result=take(&image_a);
    if(!result && !used_record(&image_a,owned_address,owned_bytes)) result=integrity_error(D88_MEMORY_HEAP_E_OWNER);
    if(!result) {
        FREE(owned_address);result=take(&image_b);
        if(result || !release_preserved(&image_a,&image_b,owned_address,owned_bytes))
            result=integrity_error(D88_MEMORY_HEAP_E_RESTORE);
        else owned_address=owned_bytes=0;
    }
    W32(NATIVE_ERRNO,saved_errno);
    if(R32(NATIVE_ERRNO)!=saved_errno) result=integrity_error(D88_MEMORY_HEAP_E_RESTORE);
    return leave(result);
}
int d88_memory_heap_owns(unsigned address,unsigned bytes)
{
    return !heap_fault && owned_address && bytes && address>=owned_address &&
           address-owned_address<=owned_bytes && bytes<=owned_bytes-(address-owned_address);
}
unsigned d88_memory_heap_fault(void) {return heap_fault;}
