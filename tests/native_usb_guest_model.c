/* Guest-only USB window and Misc single-write protection for integration.
 * The Misc filter enforces the silicon unlock contract absent in QEMU;
 * accepted writes and all ROS operations use the captured board/kernel.
 * This does not emulate a host, USB wiring, timing, or endpoint traffic. */
typedef unsigned int u32;
typedef unsigned char u8;
static u8 regs[256] = {[0x20]=0x12,[0x22]=0x40,[0x23]=0x81,[0x3f]=8};
static u8 sram[1024];
static unsigned descriptor_dummy;
volatile unsigned guest_model_counts[8];
volatile unsigned guest_model_fault[4];
volatile unsigned guest_model_vbus=1u;
/* Native ROS counter timestamps, not milliseconds: last detach, attach,
 * last attached->detached transition, native task resume. */
volatile unsigned guest_model_signals[4];
static unsigned misc_write_key;
/* keys, accepted writes, rejected writes, writes outside CPU lock. */
volatile unsigned guest_misc_counts[4];
/* CLK rise tick, SNZ release tick, rises, minimum powerup interval,
 * early releases, completed powerup intervals. Values use native ROS units. */
volatile unsigned guest_misc_clock_signals[6]={0,0,0,0xffffffffu,0,0};
static unsigned misc_powerup_pending;
static unsigned current_psr(void)
{
    unsigned value;
    __asm__ volatile("ld.w %0, %%psr" : "=r"(value) : : "memory");
    return value;
}
extern int usb_platform_get_state(void);

static void observe8(u32 address,u8 value)
{
    int bad=0;
    if(usb_platform_get_state()!=2 || guest_model_fault[0]) return;
    if(address==0x00048133u) bad=(value&0x11u)!=0x11u;
    else if(address==0x00048132u) bad=(value&0x11u)!=0;
    else if(address==0x00048124u) bad=(value&0x47u)!=0x42u;
    else if(address==0x0004812eu) bad=!(value&1u);
    else if(address==0x00300f35u) bad=(value&0x80u)!=0;
    else if(address==0x00300f20u) bad=(value&0x30u)!=0x10u;
    else if(address==0x000402a6u) bad=(value&0x40u)!=0;
    else if(address==0x02000351u) bad=value!=0;
    else if(address==*(volatile u32*)0x022a9820u+3u*72u+0x3fu) bad=!(value&8u);
    if(bad) {guest_model_fault[0]=address;guest_model_fault[1]=value;}
}
static void observe32(u32 address,u32 value)
{
    int bad=0;
    if(usb_platform_get_state()!=2 || guest_model_fault[0]) return;
    if(address==0x024ef004u || address==0x02049798u || address==0x020497e0u) bad=value!=0;
    else if(address==0x0204979cu) bad=value!=0x40000u;
    if(bad) {guest_model_fault[0]=address;guest_model_fault[1]=value;}
}

static int is_usb(u32 address)
{ return address>=0x01000000u && address<0x01000100u; }
u8 usb_test_read(unsigned address)
{
    /* Keep the index explicit: folding regs+(address-USB_BASE) into a biased
     * image symbol would violate the resident loader's closed-image audit. */
    volatile unsigned off;
    unsigned index;
    if(!is_usb(address)) {u8 value=*(volatile u8*)address;observe8(address,value);return value;}
    guest_model_counts[0]++;
    off=address-0x01000000u;
    if(off==0x22u) return (regs[off]&0x7fu)|(guest_model_vbus?0x80u:0u);
    if(off==0x8fu) {
        if(descriptor_dummy) {descriptor_dummy=0;return 0;}
        index=(((unsigned)regs[0x88]<<8)|regs[0x89])&1023u;
        regs[0x88]=(u8)((index+1u)>>8);regs[0x89]=(u8)(index+1u);
        return sram[index];
    }
    return regs[off];
}
void usb_test_write(unsigned address,u8 value)
{
    volatile unsigned off;
    unsigned index;
    if(address==0x00300f2fu) {
        misc_write_key=value==0x96u;
        guest_misc_counts[0]++;
        *(volatile u8*)address=value;
        return;
    }
    if(address==0x00300f20u || (address>=0x00300f30u && address<=0x00300f39u)) {
        u8 before;
        if(!misc_write_key) {guest_misc_counts[2]++;return;}
        misc_write_key=0;
        guest_misc_counts[1]++;
        if((*(volatile u8*)0x0206866du&3u)!=3u || (current_psr()&0x10u))
            guest_misc_counts[3]++;
        before=*(volatile u8*)address;
        if(address==0x00300f20u) {
            unsigned tick=*(volatile u32*)0x02068674u;
            if(!(before&0x10u) && (value&0x10u)) {
                guest_misc_clock_signals[0]=tick;guest_misc_clock_signals[2]++;
                misc_powerup_pending=1;
            }
            if(misc_powerup_pending && (before&0x20u) && !(value&0x20u) && (value&0x10u)) {
                unsigned elapsed=tick-guest_misc_clock_signals[0];
                guest_misc_clock_signals[1]=tick;guest_misc_clock_signals[5]++;
                if(elapsed<guest_misc_clock_signals[3]) guest_misc_clock_signals[3]=elapsed;
                if(elapsed<64u) guest_misc_clock_signals[4]++;
                misc_powerup_pending=0;
            }
            if(!(value&0x10u)) misc_powerup_pending=0;
        }
        *(volatile u8*)address=value;
        return;
    }
    if(!is_usb(address)) {*(volatile u8*)address=value;return;}
    guest_model_counts[1]++;
    off=address-0x01000000u;
    if(off==0x23u) {
        unsigned tick=*(volatile u32*)0x02068674u;
        if(!(value&0x80u)) {
            guest_model_signals[0]=tick;
            if(regs[off]&0x80u) guest_model_signals[2]=tick;
        } else if(value==0x80u) {
            guest_model_signals[1]=tick;
            guest_model_counts[7]++;
        }
    }
    if(off==0x8fu) {
        index=(((unsigned)regs[0x88]<<8)|regs[0x89])&1023u;
        sram[index]=value;
        regs[0x88]=(u8)((index+1u)>>8);regs[0x89]=(u8)(index+1u);
        return;
    }
    if(off==0 || off==1 || off==3 || off==4 || (off>=7 && off<12)) {
        regs[off]&=(u8)~value;return;
    }
    regs[off]=value;
    if(off==0x88 || off==0x89) descriptor_dummy=1;
}
void usb_test_clear_bit(unsigned address,unsigned bit)
{
    if(bit<8u) usb_test_write(address,(u8)(usb_test_read(address)&~(1u<<bit)));
}
u8 usb_platform_test_read8(unsigned address) {return usb_test_read(address);}
u32 usb_platform_test_read32(unsigned address)
{
    if(!is_usb(address)) {u32 value=*(volatile u32*)address;observe32(address,value);return value;}
    return (u32)usb_test_read(address)|((u32)usb_test_read(address+1u)<<8)|
           ((u32)usb_test_read(address+2u)<<16)|((u32)usb_test_read(address+3u)<<24);
}
void usb_platform_test_write8(unsigned address,u8 value)
{usb_test_write(address,value);}
void usb_platform_test_write32(unsigned address,u32 value)
{*(volatile u32*)address=value;}
int usb_platform_test_syscall(unsigned operation,int argument)
{
    u32 address;
    if(operation==1u) address=0x021ce8aeu;
    else if(operation==2u) address=0x021ce8d8u;
    else if(operation==3u) address=0x021cfe60u;
    else if(operation==4u) address=0x021cfed2u;
    else return -1;
    guest_model_counts[operation+1u]++;
    if(operation==4u) guest_model_signals[3]=*(volatile u32*)0x02068674u;
    if(operation>=3u) return ((int(*)(int))address)(argument);
    return ((int(*)(void))address)();
}
