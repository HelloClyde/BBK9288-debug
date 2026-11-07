/* Captured 0215EBC4 firmware only. Native GUI+64C points at a ten-byte
 * leaf that writes aligned u32[024EEF54]=0 and returns. Its timer ISR uses
 * this same counter; the final GUI shutdown path checks it again.
 * SDK tail offsets describe another table layout and must not be used. */
#include "power_idle.h"

typedef char d88_power_word_is_32[(sizeof(unsigned)==4u)?1:-1];
typedef char d88_power_diag_is_16[(sizeof(d88_power_idle_diagnostic)==16u)?1:-1];

#define GUI_ROOT 0x02000204u
#define GUI_TABLE 0x021e7b54u
#define RESET_SLOT (GUI_TABLE+0x64cu)
#define RESET_ENTRY 0x02198c94u
#define ROS_ROOT 0x02000200u
#define ROS_TABLE 0x021e81e8u
#define LOCK_ENTRY 0x021ce8aeu
#define UNLOCK_ENTRY 0x021ce8d8u
#define PENDING_REQUEST 0x02049684u

#ifdef D88_POWER_IDLE_TEST
extern unsigned d88_power_test_read32(unsigned address);
extern unsigned char d88_power_test_read8(unsigned address);
extern void d88_power_test_reset(void);
extern int d88_power_test_lock(void);
extern int d88_power_test_unlock(void);
extern unsigned d88_power_test_psr(void);
#define READ32(a) d88_power_test_read32(a)
#define READ8(a) d88_power_test_read8(a)
#define NATIVE_RESET() d88_power_test_reset()
#define LOCK() d88_power_test_lock()
#define UNLOCK() d88_power_test_unlock()
#define PSR() d88_power_test_psr()
#else
#define READ32(a) (*(volatile unsigned *)(a))
#define READ8(a) (*(volatile unsigned char *)(a))
#define NATIVE_RESET() ((void(*)(void))RESET_ENTRY)()
#define LOCK() ((int(*)(void))LOCK_ENTRY)()
#define UNLOCK() ((int(*)(void))UNLOCK_ENTRY)()
static unsigned current_psr(void)
{
    unsigned psr;
    __asm__ volatile("ld.w %0, %%psr" : "=r"(psr) : : "memory");
    return psr;
}
#define PSR() current_psr()
#endif

volatile d88_power_idle_diagnostic power_idle_diagnostic={D88_POWER_IDLE_DIAG_VERSION,0,0,0};

static int fail(int result)
{
    power_idle_diagnostic.active=0;
    power_idle_diagnostic.last_error=(unsigned)result;
    return result;
}
static int profile(void)
{
    static const unsigned char signature[10]={0x04,0x6c,0x77,0xd2,0x54,0xcf,0xf4,0x3c,0x40,0x06};
    unsigned i;
    if(READ32(GUI_ROOT)!=GUI_TABLE) return D88_POWER_IDLE_E_TABLE;
    if(READ32(RESET_SLOT)!=RESET_ENTRY) return D88_POWER_IDLE_E_ENTRY;
    for(i=0;i<sizeof(signature);i++)
        if(READ8(RESET_ENTRY+i)!=signature[i]) return D88_POWER_IDLE_E_CODE;
    if(READ32(ROS_ROOT)!=ROS_TABLE ||
       READ32(ROS_TABLE+23u*4u)!=LOCK_ENTRY ||
       READ32(ROS_TABLE+24u*4u)!=UNLOCK_ENTRY) return D88_POWER_IDLE_E_ROS;
    return 0;
}
static int refresh(void)
{
    int result;
    if((PSR()&0x710u)!=0x10u || READ8(0x0206866cu) || (READ8(0x0206866du)&7u))
        return fail(D88_POWER_IDLE_E_CONTEXT);
    result=profile();
    if(result) return fail(result);
    if(LOCK()!=0) return fail(D88_POWER_IDLE_E_LOCK);
    /* Exclude an ISR raising the request between its check and reset, and
     * exclude GUI task preemption while rechecking/calling the native leaf.
     * An existing automatic or manual request is left entirely untouched. */
    result=profile();
    if(!result && READ32(PENDING_REQUEST)) result=D88_POWER_IDLE_E_PENDING;
    if(!result) {
        NATIVE_RESET();
        power_idle_diagnostic.refreshes++;
    }
    if(UNLOCK()!=0) return fail(D88_POWER_IDLE_E_UNLOCK);
    if(result) return fail(result);
    /* An IRQ may increment the aligned counter immediately after unlock.
     * Do not reject a successful refresh because a readback is nonzero. */
    power_idle_diagnostic.last_error=0;
    return 0;
}
int d88_power_idle_begin(void)
{
    int result;
    power_idle_diagnostic.active=0;
    result=refresh();
    if(result) return result;
    power_idle_diagnostic.active=1;
    return 0;
}
int d88_power_idle_poll(void)
{
    return power_idle_diagnostic.active?refresh():0;
}
void d88_power_idle_end(void)
{
    power_idle_diagnostic.active=0;
}
