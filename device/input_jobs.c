#include "input_jobs.h"
typedef unsigned int u32;
typedef unsigned char u8;
#include "profiles.h"

static u32 ros_table,ros_lock,ros_unlock;

void d88_input_bind_ros(unsigned table,unsigned lock,unsigned unlock)
{
    ros_table=table;
    ros_lock=lock;
    ros_unlock=unlock;
}

#ifdef D88_INPUT_TEST
extern u8 d88_input_test_read8(u32 address);
extern unsigned short d88_input_test_read16(u32 address);
extern u32 d88_input_test_read32(u32 address);
extern int d88_input_test_lock(void);
extern int d88_input_test_unlock(void);
extern int d88_input_test_post(u32 window,u32 message,u32 scan,u32 extra);
#define R8(a) d88_input_test_read8(a)
#define R16(a) d88_input_test_read16(a)
#define R32(a) d88_input_test_read32(a)
#define LOCK() d88_input_test_lock()
#define UNLOCK() d88_input_test_unlock()
#define POST(w,m,s) d88_input_test_post((w),(m),(s),0)
#else
#define R8(a) (*(volatile u8*)(a))
#define R16(a) (*(volatile unsigned short*)(a))
#define R32(a) (*(volatile u32*)(a))
#define LOCK() ((int(*)(void))ros_lock)()
#define UNLOCK() ((int(*)(void))ros_unlock)()
#define POST(w,m,s) ((int(*)(u32,u32,u32,u32))(original_v15()?0x0211a1f2u:0x02182c3au))((w),(m),(s),0)
#endif

static int original_v15(void) { return R32(0x02000204u)==0x02180058u; }
static u8 input_byte(u32 address) { return R8(address); }

static int input_ros_valid(int v15)
{
    const u32 *original=d88_usb_ros_original[v15?0:1];
#ifdef D88_INPUT_TEST
    if(!ros_table) return 1; /* Host tests supply their own lock callbacks. */
#endif
    if(!ros_table || R32(0x02000200u)!=ros_table ||
       R32(ros_table+23u*4u)!=ros_lock || R32(ros_table+24u*4u)!=ros_unlock) return 0;
    return (ros_lock==original[2] || d88_match_moved_ros(input_byte,original[2],ros_lock,42u)) &&
           (ros_unlock==original[3] || d88_match_moved_ros(input_byte,original[3],ros_unlock,58u));
}

static int code_crc(u32 address,unsigned size,u32 expected)
{
    u32 crc=0xffffffffu;
    unsigned index,bit;
    for(index=0;index<size;++index) {
        crc^=R8(address+index);
        for(bit=0;bit<8u;++bit) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    return ~crc==expected;
}

static const u8 active_signature[10]={0x1c,0xd0,0x74,0xc4,0xf4,0x30,0x00,0xc0,0xcd,0x1e};
static const u8 focus_signature[26]={0x00,0xc0,0x00,0xc0,0x41,0x1c,0x04,0x68,0x07,0x18,0xc4,0xc0,0x45,0x30,0x05,0x68,0x04,0x18,0x03,0x1f,0x54,0x2e,0x04,0x6c,0x40,0x06};
static const u8 desktop_signature[8]={0x1c,0xd0,0xd0,0xc3,0xf4,0x22,0x40,0x06};
/* Whole bounded PostMessage and both callees, including their call sites.
 * A table pointer alone would not detect an application's code hook. */
static const u8 post_signature[144]={
    0x03,0x02,0x01,0x84,0x61,0x2e,0x72,0x2e,0x09,0x5c,0xf8,0xdf,0xfb,0xdf,0x7d,0x1d,
    0x83,0x2e,0x40,0x2e,0x00,0x68,0x3a,0x19,0xf4,0x6f,0x02,0xc0,0x12,0x6b,0x06,0x1a,
    0x04,0x30,0x40,0xc0,0x00,0xc0,0x04,0x74,0x2f,0x1e,0x1c,0xc0,0x06,0x30,0x14,0xc0,
    0x07,0x30,0x00,0xc0,0x92,0xc2,0x58,0x1d,0x16,0x60,0x18,0xc0,0x05,0x30,0x54,0x2a,
    0x25,0x19,0xe4,0x6f,0x1c,0xc0,0x05,0x30,0x54,0x2e,0x34,0x8c,0x54,0x26,0x24,0x8c,
    0x10,0xc0,0x05,0x30,0x54,0x22,0x41,0x3c,0x04,0xc0,0x42,0x3c,0x08,0xc0,0x43,0x3c,
    0x05,0x50,0x0c,0xc0,0x45,0x3c,0x1c,0xc0,0x04,0x30,0x14,0x60,0x1c,0xc0,0x04,0x3c,
    0x14,0xc0,0x05,0x30,0x54,0x2a,0x04,0x0d,0x04,0x6c,0x1c,0xc0,0x04,0x3c,0x04,0x30,
    0x00,0xc8,0x00,0xc0,0x04,0x74,0x04,0x3c,0x04,0x6c,0x01,0x80,0x43,0x02,0x40,0x06
};
static const u8 lookup_signature[64]={
    0x06,0x68,0x04,0x1b,0xf6,0x6b,0x10,0x1f,0xf4,0x6f,0x05,0x19,0x65,0x2e,0x54,0x28,
    0x14,0x68,0x03,0x18,0x09,0x1f,0x04,0x6c,0x02,0xc0,0x54,0x28,0x14,0x69,0x04,0x19,
    0x54,0x2e,0xb8,0xc0,0x64,0x30,0x40,0x06,0x06,0x68,0x07,0x18,0xf8,0xdf,0xff,0xdf,
    0xe8,0x1c,0xdc,0xc0,0x44,0x30,0x04,0x1e,0x1c,0xd0,0xd0,0xc3,0xf4,0x22,0x40,0x06
};
static const u8 remainder_signature[38]={
    0x62,0xa0,0x70,0x8b,0x48,0x6c,0x09,0xa4,0x70,0x93,0x70,0x93,0x70,0x93,0x70,0x93,
    0x70,0x93,0x70,0x93,0x70,0x93,0x70,0x93,0x18,0x64,0xf7,0x1b,0x90,0xa0,0x70,0x97,
    0x00,0x9b,0x40,0x07,0x34,0xa4
};
static unsigned stage,scan_code,target_set;
static u32 target,target_owner,target_queue,target_kind;
static int last_error;

static int ram(u32 address,u32 size)
{
    return !(address&3u) && address>=0x02000000u && address<0x02800000u && size<=0x02800000u-address;
}
static int signature(u32 address,const u8 *bytes,unsigned size)
{
    unsigned i;
    for(i=0;i<size;i++) if(R8(address+i)!=bytes[i]) return 0;
    return 1;
}
int d88_input_available(void)
{
    if(original_v15()) {
        /* PostMessage and its queue lookup have the same structure and
         * record offsets as the captured V1.3 implementation. Their
         * embedded addresses and the ROS lock pair differ. */
        return input_ros_valid(1) && R32(0x02180058u+60u)==0x0211a1f2u &&
            R32(0x02180058u+78u*4u)==0x02113c90u &&
            code_crc(0x0211a1f2u,144u,0x19a29ac4u) && code_crc(0x021198d2u,64u,0x5d8a3bfeu) &&
            code_crc(0x02118576u,10u,0x1763891eu) && code_crc(0x02113c90u,26u,0x873b4088u) &&
            code_crc(0x0216d578u,38u,0x59b86e71u) && code_crc(0x02166d06u,42u,0x733bd9d6u) &&
            code_crc(0x02166d30u,58u,0xfb7a4190u);
    }
    return R32(0x02000204u)==0x021e7b54u && input_ros_valid(0) && R32(0x021e7b54u+60u)==0x02182c3au &&
           R32(0x021faa7cu)==0x02180fbeu &&
           signature(0x02180fbeu,active_signature,sizeof(active_signature)) &&
           signature(0x0217c6d8u,focus_signature,sizeof(focus_signature)) &&
           signature(0x02182352u,desktop_signature,sizeof(desktop_signature)) &&
           signature(0x02182c3au,post_signature,sizeof(post_signature)) &&
           signature(0x0218231au,lookup_signature,sizeof(lookup_signature)) &&
           signature(0x021d5120u,remainder_signature,sizeof(remainder_signature));
}
static int main_window(u32 window)
{
    return ram(window,0xe0u) && R16(window)==1u && R16(window+2u)==0x11u;
}
static u32 message_queue(u32 window,u32 *main)
{
    u32 owner;
    if(main) *main=0;
    if(!window) return original_v15()?0x0227923cu:0x020383d0u;
    if(!ram(window,0xbcu) || R16(window)!=1u) return 0;
    owner=R16(window+2u)==0x11u?window:R32(window+0xb8u);
    if(!main_window(owner)) return 0;
    if(main) *main=owner;
    return R32(owner+0xdcu);
}
static int queue_valid(u32 queue)
{
    u32 count,ring,read_index,write_index;
    if(!ram(queue,32u)) return 0;
    ring=R32(queue+0x10u);count=R32(queue+0x14u);
    read_index=R32(queue+0x18u);write_index=R32(queue+0x1cu);
    return count>=2u && count<=1024u && read_index<count && write_index<count && ram(ring,count*28u);
}
static int choose_target(u32 *window)
{
    u32 active=R32(original_v15()?0x022792e0u:0x02038474u),focus,owner;
    if(!active) {*window=0;return 1;}
    if(!main_window(active)) return 0;
    *window=active;focus=R32(active+0xc4u);
    if(focus) {
        if(!ram(focus,0xbcu) || R16(focus)!=1u) return 0;
        owner=R16(focus+2u)==0x11u?focus:R32(focus+0xb8u);
        if(owner!=active) return 0;
        *window=focus;
    }
    return 1;
}
void d88_input_reset(void)
{stage=scan_code=target_set=0;target=target_owner=target_queue=target_kind=0;last_error=0;}
void d88_input_cancel(void)
{
    if(stage==1u) {stage=scan_code=target_set=0;target=0;}
}
int d88_input_queue(unsigned scan)
{
    if(!scan || scan>127u || !d88_input_available()) return -1;
    if(stage) return -2;
    stage=1;scan_code=scan;target_set=0;last_error=0;return 0;
}
int d88_input_poll(void)
{
    int result,unlock;
    u32 owner,queue,kind;
    if(!stage) return 0;
    if(!d88_input_available()) {last_error=-3;return -3;}
    if(LOCK()!=0) {last_error=-3;return -3;}
    /* A GUI task could patch native code between the first check and LOCK. */
    if(!d88_input_available()) {last_error=-3;(void)UNLOCK();return -3;}
    if(!target_set) {
        if(!choose_target(&target)) {
            stage=0;last_error=UNLOCK()!=0?-3:-1;return last_error;
        }
    }
    /* Native PostMessage dereferences owner/queue without a null check.
     * Check the full bounded chain while GUI and interrupts are paused. */
    queue=message_queue(target,&owner);
    kind=target && queue?R16(target+2u):0;
    if(!queue_valid(queue) || (target_set &&
       (owner!=target_owner || queue!=target_queue || kind!=target_kind))) {
        stage=0;last_error=UNLOCK()!=0?-3:-1;return last_error;
    }
    target_owner=owner;target_queue=queue;target_kind=kind;target_set=1;
    result=POST(target,stage==1u?0x10u:0x12u,scan_code);
    if(result==0) {
        if(stage==1u) stage=2;
        else {stage=0;scan_code=target_set=0;}
    } else if(result!=-2) {stage=0;last_error=result;}
    unlock=UNLOCK();
    if(unlock!=0) {last_error=-3;return -3;}
    return result;
}
int d88_input_pending(void) {return stage!=0;}
int d88_input_last_error(void) {return last_error;}
