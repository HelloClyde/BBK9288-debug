/* Production input_jobs.c with independently seeded captured-code bytes and a
 * GUI ring model. No native firmware, operating system or physical input runs. */
#include <assert.h>
#include <string.h>
#define D88_INPUT_TEST 1
#include "../device/input_jobs.c"

#define RAM_BASE 0x02000000u
#define MODEL_MAIN 0x02501000u
#define MODEL_FOCUS 0x02502000u
#define MODEL_OTHER 0x02503000u
#define MODEL_QUEUE 0x02504000u
#define MODEL_OTHER_QUEUE 0x02504100u
#define MODEL_RING 0x02505000u
#define MODEL_OTHER_RING 0x02506000u
/* Four unused padding bytes avoid GCC's interprocedural array-bounds false
 * positive after it inlines the assert-guarded owner+2 address lookup. */
static u8 memory[0x00800004u];
static u32 attempts[128][4],accepted[128][4];
static unsigned attempt_count,accepted_count,lock_calls,unlock_calls;
static int lock_depth,fail_lock,fail_unlock,post_result;
static u32 corrupt_on_lock;

static u8 *at(u32 address,unsigned size)
{
    assert(address>=RAM_BASE && address<0x02800000u);
    assert(size<=0x02800000u-address);
    return memory+address-RAM_BASE;
}
static u32 get32(u32 a)
{
    const u8 *p=at(a,4);
    return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
}
static void set32(u32 a,u32 value)
{
    u8 *p=at(a,4);
    p[0]=(u8)value;p[1]=(u8)(value>>8);p[2]=(u8)(value>>16);p[3]=(u8)(value>>24);
}
u8 d88_input_test_read8(u32 address) {return *at(address,1);}
unsigned short d88_input_test_read16(u32 address)
{
    u8 *p=at(address,2);
    assert(lock_depth==1); /* Window fields are never read outside the lock. */
    return (unsigned short)((unsigned)p[0]|((unsigned)p[1]<<8));
}
u32 d88_input_test_read32(u32 address)
{
    if(address!=0x02000204u && address!=0x021e7b90u && address!=0x021faa7cu)
        assert(lock_depth==1);
    return get32(address);
}
int d88_input_test_lock(void)
{
    lock_calls++;
    if(fail_lock) {fail_lock=0;return -9;}
    assert(!lock_depth);lock_depth=1;
    if(corrupt_on_lock) {*at(corrupt_on_lock,1)=0xa5;corrupt_on_lock=0;}
    return 0;
}
int d88_input_test_unlock(void)
{
    unlock_calls++;assert(lock_depth==1);lock_depth=0;
    if(fail_unlock) {fail_unlock=0;return -9;}
    return 0;
}
int d88_input_test_post(u32 window,u32 message,u32 scan,u32 extra)
{
    u32 owner,queue,ring,count,read_index,write_index,next;
    assert(lock_depth==1);assert(attempt_count<128u);
    attempts[attempt_count][0]=window;attempts[attempt_count][1]=message;
    attempts[attempt_count][2]=scan;attempts[attempt_count++][3]=extra;
    if(post_result) return post_result;
    if(!window) queue=0x020383d0u;
    else {
        const u8 *p=at(window,4);
        assert(p[0]==1u && !p[1]);
        owner=p[2]==0x11u && !p[3]?window:get32(window+0xb8u);
        assert(*at(owner,1)==1u && *at(owner+2u,1)==0x11u);
        queue=get32(owner+0xdcu);
    }
    ring=get32(queue+0x10u);count=get32(queue+0x14u);
    read_index=get32(queue+0x18u);write_index=get32(queue+0x1cu);
    assert(count>=2u && count<=1024u && read_index<count && write_index<count);
    (void)at(ring,count*28u);
    next=(write_index+1u)%count;
    if(next==read_index) return -2;
    set32(ring+write_index*28u,window);set32(ring+write_index*28u+4u,message);
    set32(ring+write_index*28u+8u,scan);set32(ring+write_index*28u+12u,extra);
    set32(queue+0x1cu,next);
    assert(accepted_count<128u);
    memcpy(accepted[accepted_count++],attempts[attempt_count-1u],sizeof(attempts[0]));
    return 0;
}
static void bytes(u32 address,const char *hex)
{
    while(*hex) {
        unsigned hi=*hex<='9'?(unsigned)(*hex-'0'):(unsigned)(*hex-'a'+10);hex++;
        unsigned lo=*hex<='9'?(unsigned)(*hex-'0'):(unsigned)(*hex-'a'+10);hex++;
        *at(address++,1)=(u8)((hi<<4)|lo);
    }
}
static void queue_init(u32 queue,u32 ring)
{set32(queue+0x10u,ring);set32(queue+0x14u,8);set32(queue+0x18u,0);set32(queue+0x1cu,0);}
void model_reset(void)
{
    memset(memory,0,sizeof(memory));memset(attempts,0,sizeof(attempts));memset(accepted,0,sizeof(accepted));
    attempt_count=accepted_count=lock_calls=unlock_calls=0;
    lock_depth=fail_lock=fail_unlock=post_result=0;corrupt_on_lock=0;d88_input_reset();
    set32(0x02000204u,0x021e7b54u);set32(0x021e7b90u,0x02182c3au);set32(0x021faa7cu,0x02180fbeu);
    bytes(0x02180fbeu,"1cd074c4f43000c0cd1e");
    bytes(0x0217c6d8u,"00c000c0411c04680718c4c0453005680418031f542e046c4006");
    bytes(0x0218231au,"0668041bf66b101ff46f0519652e542814680318091f046c02c0542814690419542eb8c06430400606680718f8dfffdfe81cdcc04430041e1cd0d0c3f4224006");
    bytes(0x021d5120u,"62a0708b486c09a4709370937093709370937093709370931864f71b90a07097009b400734a4");
    bytes(0x02182c3au,
        "03020184612e722e095cf8dffbdf7d1d832e402e00683a19f46f02c0126b061a"
        "043040c000c004742f1e1cc0063014c0073000c092c2581d166018c00530542a"
        "2519e46f1cc00530542e348c5426248c10c005305422413c04c0423c08c0433c"
        "05500cc0453c1cc0043014601cc0043c14c00530542a040d046c1cc0043c0430"
        "00c800c00474043c046c018043024006");
    set32(MODEL_MAIN,0x00110001u);set32(MODEL_OTHER,0x00110001u);set32(MODEL_FOCUS,0x00120001u);
    set32(MODEL_MAIN+0xdcu,MODEL_QUEUE);set32(MODEL_OTHER+0xdcu,MODEL_OTHER_QUEUE);
    set32(MODEL_FOCUS+0xb8u,MODEL_MAIN);
    queue_init(MODEL_QUEUE,MODEL_RING);queue_init(MODEL_OTHER_QUEUE,MODEL_OTHER_RING);
    queue_init(0x020383d0u,0x02507000u);
    set32(0x02038474u,MODEL_MAIN);
}
void model_write32(u32 address,u32 value) {set32(address,value);}
void model_write8(u32 address,u32 value) {*at(address,1)=(u8)value;}
u32 model_read32(u32 address) {return get32(address);}
unsigned model_attempts(void) {return attempt_count;}
unsigned model_accepted(void) {return accepted_count;}
u32 model_attempt(unsigned index,unsigned field) {assert(index<attempt_count && field<4);return attempts[index][field];}
u32 model_accepted_field(unsigned index,unsigned field) {assert(index<accepted_count && field<4);return accepted[index][field];}
int model_lock_depth(void) {return lock_depth;}
unsigned model_lock_calls(void) {return lock_calls;}
unsigned model_unlock_calls(void) {return unlock_calls;}
void model_fail_lock(void) {fail_lock=1;}
void model_fail_unlock(void) {fail_unlock=1;}
void model_post_result(int result) {post_result=result;}
void model_corrupt_on_lock(u32 address) {corrupt_on_lock=address;}
