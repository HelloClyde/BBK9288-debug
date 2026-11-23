/* Runs only as a debugger-injected fixture in a disposable emulator image. */
typedef unsigned int u32;
typedef unsigned char u8;
#include "../device/profiles.h"

#define REPORT ((volatile u32 *)0x02708000u)
#define BYTE(address) (*(volatile u8 *)(address))
#define WORD(address) (*(volatile u32 *)(address))

static u32 read_word(u32 address) { return WORD(address); }
static u8 read_byte(u32 address) { return BYTE(address); }
extern void usb_platform_timer_guard(void);

__attribute__((noreturn,used))
void run_usb_ros_test(void)
{
    const probe_profile *profile=d88_match_firmware(read_word,read_byte);
    d88_ros_entries ros;
    u32 tcb, task, psr;
    __asm__ volatile("ld.w %0, %%psr" : "=r"(psr));
    REPORT[0]=1;
    if(profile && profile->id==1u && (psr&0x710u)==0x10u &&
       d88_resolve_usb_ros(profile,read_word,read_byte,0,&ros)) {
        tcb=WORD(profile->tcb_pointer);
        task=tcb+3u*72u;
        if(tcb==0x022a9ae0u && BYTE(task+63u)==7u &&
           WORD(task+24u)==profile->signature_addresses[2] &&
           WORD(0x020000a8u)==profile->usb_timer_entry &&
           ((int(*)(void))ros.lock)()==0) {
            u8 timer=BYTE(0x00040273u),usb=BYTE(0x000402a6u);
            int suspended=((int(*)(int))ros.suspend)(3);
            REPORT[1]=(u32)suspended;
            REPORT[2]=BYTE(task+63u);
            if((REPORT[2]&8u)!=0u) {
                BYTE(0x00040273u)=timer&~0x40u;
                BYTE(0x000402a6u)=usb&~0x40u;
                WORD(0x020000a8u)=(u32)usb_platform_timer_guard;
                REPORT[3]=WORD(0x020000a8u);
                WORD(0x020000a8u)=profile->usb_timer_entry;
                REPORT[4]=(u32)((int(*)(int))ros.resume)(3);
                BYTE(0x00040273u)=timer;
                BYTE(0x000402a6u)=usb;
                REPORT[5]=BYTE(task+63u);
                REPORT[6]=WORD(0x020000a8u);
                REPORT[7]=BYTE(0x00040273u)==timer && BYTE(0x000402a6u)==usb;
            }
            REPORT[8]=(u32)((int(*)(void))ros.unlock)();
            REPORT[0]=2;
        }
    }
    for(;;) __asm__ volatile("nop");
}
