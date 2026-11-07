/* Once-only native desktop callback. Disable the callback before the launcher's
 * MessageBox can dispatch another desktop message. Only used in offline QEMU. */
extern long DL_AppMain(long argument);
volatile unsigned guest_hook_status=0x12345678u;
volatile long guest_hook_result=0x12345678;
long guest_launcher_hook(unsigned a,unsigned b,unsigned c,unsigned d)
{
    (void)a;(void)b;(void)c;(void)d;
    *(volatile unsigned*)0x0203857cu=0;
    guest_hook_status=1;
    guest_hook_result=DL_AppMain(0);
    guest_hook_status=2;
    return 0;
}
