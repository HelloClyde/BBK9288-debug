/* Offline model mailbox. Invoked from guest USB reads in real task8, never
 * from a physical transport. The production file/GUI/FS/ROS code is unchanged.
 * after_gui_job is inserted only in the model build, after busy has cleared. */
#include "file_runtime.h"
#include "gui_jobs.h"
volatile unsigned guest_file_mailbox[8];
unsigned char guest_file_input[512],guest_file_output[512];
/* request, entered, returned, result, launcher_status_ptr, TCB, PSR, hook. */
volatile unsigned guest_file_handoff[8];
static unsigned current_psr(void)
{unsigned n;__asm__ volatile("ld.w %0, %%psr":"=r"(n)::"memory");return n;}
static int ordinary(void)
{return (current_psr()&0x710u)==0x10u && !*(volatile unsigned char*)0x0206866cu &&
        !(*(volatile unsigned char*)0x0206866du&7u);}
void guest_file_worker_tick(void)
{
    unsigned seq,length,output=0;
    int r;
    if(*(volatile unsigned*)0x02068664u!=0x02068d74u || !ordinary()) return;
    seq=guest_file_mailbox[0];
    if(seq!=guest_file_mailbox[1]) {
        length=guest_file_mailbox[3];
        r=length<=sizeof(guest_file_input)?
            d88_files_rpc(0,guest_file_mailbox[2],guest_file_input,length,guest_file_output,&output):-99;
        guest_file_mailbox[4]=(unsigned)r;guest_file_mailbox[5]=output;
        guest_file_mailbox[6]=*(volatile unsigned*)0x02068664u;
        guest_file_mailbox[7]=current_psr();guest_file_mailbox[1]=seq;
    }
    if(guest_file_handoff[0]) {
        (void)d88_gui_jobs_request();(void)d88_gui_jobs_poll();
    }
}
void guest_after_gui_job(void)
{
    unsigned status;
    long result;
    if(!guest_file_handoff[0] || *(volatile unsigned*)0x02068664u!=0x02068b7cu ||
       !ordinary() || !d88_gui_jobs_idle()) return;
    guest_file_handoff[0]=0;guest_file_handoff[1]++;
    guest_file_handoff[5]=*(volatile unsigned*)0x02068664u;
    guest_file_handoff[6]=current_psr();guest_file_handoff[7]=*(volatile unsigned*)0x0203857cu;
    status=guest_file_handoff[4];
    if(status<0x02700000u || status>=0x02800000u || (status&3u)) return;
    *(volatile unsigned*)status=1u;
    result=((long(*)(long))0x02700000u)(0);
    guest_file_handoff[3]=(unsigned)result;guest_file_handoff[2]++;
    *(volatile unsigned*)status=2u;
}
