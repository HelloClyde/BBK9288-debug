/* S1C33L05 manual VI-2: USB register map, CPU FIFO interface,
 * descriptor reply, and AutoSetAddress. See ../docs/usb-platform.md.
 * This driver deliberately does not infer native firmware USB ownership.
 */
#include "usb_cdc.h"

#ifndef D9288_USB_PLATFORM_VERIFIED
#define D9288_USB_PLATFORM_VERIFIED 0
#endif
#ifndef D9288_USB_TIMER_GUARDED
#define D9288_USB_TIMER_GUARDED 0
#endif

#ifdef USB_CDC_TEST
extern unsigned char usb_test_read(unsigned address);
extern void usb_test_write(unsigned address, unsigned char value);
extern void usb_test_clear_bit(unsigned address, unsigned bit);
#define RD(a) usb_test_read(a)
#define WR(a,v) usb_test_write((a),(unsigned char)(v))
#else
#define RD(a) (*(volatile unsigned char *)(a))
#define WR(a,v) (*(volatile unsigned char *)(a) = (unsigned char)(v))
#endif
#define BASE 0x01000000u
#define R(n) RD(BASE+(n))
#define W(n,v) WR(BASE+(n),(v))
#define MIN(a,b) ((a)<(b)?(a):(b))

enum { MAIN=0, SIE=1, FIFO_STAT=4, EP0_STAT=7, EPA_STAT=8, EPB_STAT=9,
       USB_CTRL=0x21, USB_STATUS=0x22, XCVR=0x23, EP_ALL=0x25,
       EP_CLEAR=0x26, ADDRESS=0x38, EP0_CTRL=0x39, EP0_IN=0x3a,
       EP0_OUT=0x3b, EP0_MAX=0x3f, EPA_CTRL=0x40, EPB_CTRL=0x41,
       EPC_CTRL=0x42, JOIN_RD=0x80, JOIN_WR=0x81, CPU_ACCESS=0x82,
       CPU_DATA=0x83, REMAIN_RD=0x84, REMAIN_WR=0x86, DESC_ADDR=0x88,
       DESC_SIZE=0x8a, DESC_DOOR=0x8f };
enum { ST_IDLE, ST_DATA_IN, ST_STATUS_OUT, ST_LINE_OUT, ST_STATUS_IN };
static unsigned char running, attached, configuration, dtr, ctrl_state, pending_config, pending_dtr, pending_interface, vbus;
static unsigned char line_coding[7] = {0x00,0xc2,0x01,0,0,0,8};
static unsigned char ctrl_reply[128];
static unsigned char saved_usb_ctrl, saved_xcvr;
static unsigned char tx_pending, tx_need_zlp;
static unsigned char bulk_quiesced;
static unsigned session;
volatile usb_cdc_diagnostic usb_diagnostic={.version=USB_CDC_DIAG_VERSION};

static const unsigned char device_descriptor[] = {
    /* Single CDC ACM function: device-level 02/02/01 matches Windows'
     * inbox usbser.inf directly; 02/00/00 matches neither usbser nor the
     * automatic USB\\COMPOSITE parent-driver criteria. */
    18,1,0x00,0x02,2,2,1,64,0x09,0x12,0x88,0x92,0,1,1,2,3,1
};
static const unsigned char config_descriptor[] = {
    9,2,67,0,2,1,0,0x80,50,
    9,4,0,0,1,2,2,1,0,
    5,0x24,0,0x10,0x01, 5,0x24,1,0,1,
    4,0x24,2,2, 5,0x24,6,0,1,
    7,5,0x83,3,16,0,16,
    9,4,1,0,2,0x0a,0,0,0,
    7,5,0x02,2,64,0,0, 7,5,0x81,2,64,0,0
};
static const unsigned char languages[] = {4,3,9,4};

static unsigned get_pair(unsigned off)
{
    /* High then low: the controller latches the count for the pair. */
    unsigned hi=R(off), lo=R(off+1);
    return ((hi&15u)<<8)|lo;
}
static void set_pair(unsigned off, unsigned value)
{
    W(off,value>>8); W(off+1,value);
}
static unsigned little16(const unsigned char *p)
{ return (unsigned)p[0]|((unsigned)p[1]<<8); }

static void stall(void)
{
    W(EP0_IN,1); W(EP0_OUT,1); ctrl_state=ST_IDLE;
}
static void status_in(void)
{
    W(EP_ALL,1); /* discard previous EP0 packet before a zero-length status */
    W(EP0_CTRL,0x80); W(EP0_OUT,2); W(EP0_IN,0x48);
    ctrl_state=ST_STATUS_IN;
}
static void ep0_clear_in_nak(void)
{
#ifdef USB_CDC_TEST
    usb_test_clear_bit(BASE+EP0_IN,1u);
#else
    /* Match native 0219DFEC: change only ForceNAK, preserving the short
     * enable that the descriptor engine can set independently (VI-2-14).
     * One peripheral bit-clear instruction avoids a C read/write gap. */
    __asm__ volatile ("bclr [%0], 1" : : "r"(BASE+EP0_IN) : "memory");
#endif
}
static void reply(const unsigned char *data, unsigned size, unsigned requested)
{
    unsigned i;
    size=MIN(size,requested);
    if(!size) {
        /* All replies have a nonempty payload. wLength=0 means no data
         * stage, so the status transaction is IN regardless of bmRequest. */
        status_in(); return;
    }
    W(EP0_CTRL,0x80); /* Set IN direction separately, as the native SETUP path. */
    W(EP0_IN,0x0a); /* DATA1, keep NAK set, discard a previous short enable. */
    W(EP0_OUT,2);
    /* EP0=0..63; scratch descriptor area=64..255; bulk FIFO starts at256. */
    set_pair(DESC_ADDR,64);
    for (i=0;i<size;i++) W(DESC_DOOR,data[i]);
    set_pair(DESC_ADDR,64); set_pair(DESC_SIZE,size);
    W(EP0_CTRL,0x81);
    ep0_clear_in_nak(); /* Do not overwrite the engine's automatic EnShortPkt. */
    ctrl_state=ST_DATA_IN;
}
static void make_string(const char *s, unsigned requested)
{
    unsigned i=2;
    while (*s && i+1<sizeof(ctrl_reply)) {
        ctrl_reply[i++]=(unsigned char)*s++; ctrl_reply[i++]=0;
    }
    ctrl_reply[0]=(unsigned char)i; ctrl_reply[1]=3;
    reply(ctrl_reply,i,requested);
}
static void endpoints(unsigned enabled)
{
    unsigned i;
    configuration=(unsigned char)enabled;
    dtr=0; bulk_quiesced=0; tx_pending=tx_need_zlp=0; session++;
    W(0x52,0x81|(enabled?0x20:0));
    W(0x56,0x02|(enabled?0x20:0));
    W(0x5a,0x83|(enabled?0x20:0)); W(0x5e,0);
    W(EP_CLEAR,15);
    W(EPA_CTRL,0x06); /* clear data toggle, keep IN NAK until data queued */
    W(EPB_CTRL,0x26); /* DATA0, but no OUT acceptance before DTR status ACK. */
    W(EPC_CTRL,0x06);
    for(i=8;i<12;i++) W(i,0xff);
}
static unsigned endpoint_ctrl(unsigned address)
{
    if(address==0x81) return EPA_CTRL;
    if(address==0x02) return EPB_CTRL;
    if(address==0x83) return EPC_CTRL;
    return 0;
}
static void reset_endpoint(unsigned ep)
{
    W(ep,2); /* No transaction can race the FIFO/toggle reset. */
    W(EP_CLEAR,1u<<(ep-EPA_CTRL));
    W(ep,(ep==EPB_CTRL?(dtr && !bulk_quiesced?0x20:0x22):2)|4);
    W(EPA_STAT+(ep-EPA_CTRL),0x7f);
    if(ep==EPA_CTRL) tx_pending=tx_need_zlp=0;
}
static void control_status_ack(void)
{
    W(EP0_STAT,0x20);W(EP0_IN,2);ctrl_state=ST_IDLE;
    if(pending_config!=0xff) {endpoints(pending_config);pending_config=0xff;}
    if(pending_interface!=0xff) {
        if(pending_interface==0) reset_endpoint(EPC_CTRL);
        else {reset_endpoint(EPA_CTRL);reset_endpoint(EPB_CTRL);session++;}
        pending_interface=0xff;
    }
    if(pending_dtr!=0xff) {
        /* Old data was gated and discarded before this status was armed.
         * The SIE may already have seen a new OUT after the hardware ACK:
         * release NAK here, but never clear the receive FIFO after that ACK. */
        dtr=pending_dtr;pending_dtr=0xff;bulk_quiesced=0;
        W(EPB_CTRL,dtr?0x20:0x22);
    }
}
static void setup(void)
{
    unsigned char p[8]; unsigned i,value,index,length,ep;
    usb_diagnostic.setup_count++;
    for(i=0;i<8;i++) p[i]=R(0x30+i);
    value=little16(p+2); index=little16(p+4); length=little16(p+6);
    if(bulk_quiesced) {
        /* A new SETUP without the previous status ACK aborts that request.
         * Keep its session invalidation, restore only the old DTR level. */
        bulk_quiesced=0;W(EPB_CTRL,dtr?0x20:0x22);
    }
    W(MAIN,1); /* interrupt status registers are write-one-to-clear */
    W(EP0_STAT,0x3f); W(FIFO_STAT,0x80); W(EP_ALL,1);
    /* An intervening SETUP cancels a previous SET_ADDRESS transaction, but
     * hardware leaves its AutoSetAddress enable set (manual VI-2-58). */
    W(ADDRESS,R(ADDRESS)&127);
    ctrl_state=ST_IDLE; pending_config=0xff; pending_dtr=0xff; pending_interface=0xff;
    if(p[0]==0x80 && p[1]==6) {
        switch(value>>8) {
        case 1: if((value&255)||index) {stall();return;}
                reply(device_descriptor,sizeof(device_descriptor),length);return;
        case 2: if((value&255)||index) {stall();return;}
                reply(config_descriptor,sizeof(config_descriptor),length);return;
        case 3:
            if((value&255)==0) {reply(languages,sizeof(languages),length);return;}
            if(index!=0x409) {stall();return;}
            if((value&255)==1) {make_string("BBK development",length);return;}
            if((value&255)==2) {make_string("9288 Debug experimental",length);return;}
            if((value&255)==3) {make_string("D9288-EXPERIMENTAL-1",length);return;}
        }
    } else if(p[0]==0 && p[1]==5 && value<128 && !index && !length) {
        /* Hardware applies SET_ADDRESS only after ACK of the status IN. */
        W(ADDRESS,(R(ADDRESS)&127)|0x80); status_in(); return;
    } else if(p[0]==0 && p[1]==9 && value<=1 && !index && !length) {
        pending_config=(unsigned char)value; status_in(); return;
    } else if(p[0]==0x80 && p[1]==8 && !value && !index && length==1) {
        reply(&configuration,1,length);return;
    } else if((p[0]==0x80 || p[0]==0x81 || p[0]==0x82) && p[1]==0
              && !value && length==2) {
        ctrl_reply[0]=ctrl_reply[1]=0;
        if(p[0]==0x80 && index) {stall();return;}
        if(p[0]==0x81 && (!configuration || index>1)) {stall();return;}
        if(p[0]==0x82) {
            ep=endpoint_ctrl(index);
            if(index==0 || index==0x80) ctrl_reply[0]=(unsigned char)(R(index?EP0_IN:EP0_OUT)&1);
            else {
                if(!configuration || !ep) {stall();return;}
                ctrl_reply[0]=(unsigned char)(R(ep)&1);
            }
        }
        reply(ctrl_reply,2,length);return;
    } else if(p[0]==0x02 && (p[1]==1 || p[1]==3) && !value && !length) {
        ep=endpoint_ctrl(index);
        if(!configuration || !ep) {stall();return;}
        if(p[1]==3) W(ep,(R(ep)&0xe3)|1);
        else {
            /* A halt clear resets DATA0 and drops only this endpoint FIFO. */
            reset_endpoint(ep);
        }
        status_in();return;
    } else if(p[0]==0x81 && p[1]==10 && !value && index<2 && length==1 && configuration) {
        ctrl_reply[0]=0;reply(ctrl_reply,1,length);return;
    } else if(p[0]==0x01 && p[1]==11 && !value && index<2 && !length && configuration) {
        pending_interface=(unsigned char)index;
        status_in();return;
    } else if(p[0]==0xa1 && p[1]==0x21 && !value && !index && length==7 && configuration) {
        reply(line_coding,7,length);return;
    } else if(p[0]==0x21 && p[1]==0x20 && !value && !index && length==7 && configuration) {
        W(EP0_CTRL,0); W(EP0_IN,2); W(EP0_OUT,0x88);ctrl_state=ST_LINE_OUT;return;
    } else if(p[0]==0x21 && p[1]==0x22 && !index && !length && value<4 && configuration) {
        /* Every line-state request is a transport barrier, even a repeated
         * DTR1/RTS-only request: a Windows reopen need not send DTR0 first.
         * Force NAK before discarding old data; only then arm status IN. */
        W(EPA_CTRL,2);W(EPB_CTRL,0x22);W(EP_CLEAR,3);
        W(EPA_STAT,0x3f);W(EPB_STAT,0x7f);
        tx_pending=tx_need_zlp=0;bulk_quiesced=1;session++;
        pending_dtr=(unsigned char)(value&1);status_in();return;
    } else if(p[0]==0x21 && p[1]==0x23 && !index && !length && configuration) {
        status_in();return; /* SEND_BREAK: no UART is attached */
    }
    stall();
}

static unsigned read_platform(volatile usb_cdc_diagnostic *record)
{
    unsigned failure=0;
    record->bcu_hi=RD(0x00048133u);record->bcu_lo=RD(0x00048132u);
    record->misc_f20=RD(0x00300f20u);record->clock_f35=RD(0x00300f35u);
    record->usb_irq=RD(0x000402a6u);record->timer_irq=RD(0x00040273u);
    if((record->bcu_hi&0x11)!=0x11) failure|=USB_CDC_DIAG_BCU_HI;
    if(record->bcu_lo&0x11) failure|=USB_CDC_DIAG_BCU_LO;
    if(!(record->misc_f20&0x10)) failure|=USB_CDC_DIAG_CLOCK_MISSING;
    if(record->misc_f20&0x20) failure|=USB_CDC_DIAG_SNOOZE;
    if(record->clock_f35&0x80) failure|=USB_CDC_DIAG_MODULE_CLOCK;
    if(record->usb_irq&0x40) failure|=USB_CDC_DIAG_USB_IRQ;
    if(!D9288_USB_TIMER_GUARDED && (record->timer_irq&0x40)) failure|=USB_CDC_DIAG_TIMER_IRQ;
    return failure;
}
static void diagnostic_begin(unsigned phase)
{
    unsigned i,setup_count=usb_diagnostic.setup_count,poll_count=usb_diagnostic.poll_count;
    volatile unsigned char *byte=(volatile unsigned char*)&usb_diagnostic;
    for(i=0;i<sizeof(usb_diagnostic);i++) byte[i]=0;
    usb_diagnostic.version=USB_CDC_DIAG_VERSION;usb_diagnostic.phase=phase;
    usb_diagnostic.setup_count=setup_count;usb_diagnostic.poll_count=poll_count;
}
static int diagnostic_result(int result,unsigned failure)
{
    usb_diagnostic.result=(unsigned)result;usb_diagnostic.failure_mask=failure;
    return result;
}
static int diagnostic_usb(void)
{
    usb_diagnostic.usb_valid=0;usb_diagnostic.revision=R(0x20);
    if(usb_diagnostic.revision!=0x12) return 0;
    usb_diagnostic.control=R(USB_CTRL);usb_diagnostic.status=R(USB_STATUS);
    usb_diagnostic.xcvr=R(XCVR);usb_diagnostic.main_status=R(MAIN);
    usb_diagnostic.sie_status=R(SIE);usb_diagnostic.sie_enable=R(0x11);
    usb_diagnostic.test=R(0x24);usb_diagnostic.ep0_status=R(EP0_STAT);
    usb_diagnostic.ep0_control=R(EP0_CTRL);usb_diagnostic.ep0_in=R(EP0_IN);
    usb_diagnostic.ep0_out=R(EP0_OUT);usb_diagnostic.ep0_max=R(EP0_MAX);
    usb_diagnostic.address=R(ADDRESS);usb_diagnostic.usb_valid=1;
    return 1;
}
static void drop_transport(void)
{
    attached=configuration=dtr=tx_pending=tx_need_zlp=bulk_quiesced=0;
    ctrl_state=ST_IDLE;session++;
}
int usb_start(void)
{
    unsigned i,failure;
    diagnostic_begin(USB_CDC_DIAG_START);
    if(running) return diagnostic_result(USB_CDC_E_BUSY,USB_CDC_DIAG_BUSY);
    if(!D9288_USB_PLATFORM_VERIFIED)
        return diagnostic_result(USB_CDC_E_PLATFORM_UNVERIFIED,USB_CDC_DIAG_UNVERIFIED);
    /* This gate means firmware ownership and clock/bus setup were proven
     * separately. Never change BCU, clock source, or native IRQ masks here. */
    failure=read_platform(&usb_diagnostic);
    if(failure) return diagnostic_result(USB_CDC_E_PLATFORM_STATE,failure);
    if(!diagnostic_usb()) return diagnostic_result(USB_CDC_E_REVISION,USB_CDC_DIAG_REVISION);
    failure=(usb_diagnostic.control&1?USB_CDC_DIAG_ACTIVE:0u) |
            (usb_diagnostic.xcvr&0x80?USB_CDC_DIAG_PULLUP:0u);
    if(failure) return diagnostic_result(USB_CDC_E_BUSY,failure);
    saved_usb_ctrl=R(USB_CTRL); saved_xcvr=R(XCVR);
    /* Platform ownership is already established. Keep D+ disconnected while
     * the caller yields to ROS for the full host observation interval. */
    W(USB_CTRL,0); W(XCVR,1);
    for(i=0x10;i<0x1c;i++) W(i,0); /* PIO polling only, no CPU interrupts */
    W(EP0_MAX,64);
    set_pair(0x70,256);set_pair(0x72,512);set_pair(0x74,768);set_pair(0x76,896);
    set_pair(0x50,64);set_pair(0x54,64);set_pair(0x58,16);set_pair(0x5c,64);
    W(0x53,0);W(0x57,0);W(0x5b,0);W(0x5f,0);
    W(0x91,0);W(CPU_ACCESS,0);W(JOIN_RD,0);W(JOIN_WR,0);
    W(EP_ALL,0x20); endpoints(0); W(ADDRESS,0);
    W(EP0_CTRL,0); W(EP0_IN,2); W(EP0_OUT,2);
    W(MAIN,1);W(SIE,0xff);W(EP0_STAT,0x3f);W(FIFO_STAT,0x83);
    ctrl_state=ST_IDLE; pending_config=0xff; pending_dtr=0xff; pending_interface=0xff;
    attached=0;running=1;
    usb_diagnostic.setup_count=usb_diagnostic.poll_count=0;
    if(!diagnostic_usb()) {
        drop_transport();return diagnostic_result(USB_CDC_E_REVISION,USB_CDC_DIAG_REVISION);
    }
    return diagnostic_result(0,0);
}
int usb_attach(unsigned detached_ms)
{
    unsigned failure;
    diagnostic_begin(USB_CDC_DIAG_ATTACH);
    if(!D9288_USB_PLATFORM_VERIFIED)
        return diagnostic_result(USB_CDC_E_PLATFORM_UNVERIFIED,USB_CDC_DIAG_UNVERIFIED);
    if(!running || attached) return diagnostic_result(USB_CDC_E_BUSY,USB_CDC_DIAG_BUSY);
    failure=read_platform(&usb_diagnostic);
    if(failure) return diagnostic_result(USB_CDC_E_PLATFORM_STATE,failure);
    /* Caller supplies a verified elapsed-time lower bound, established by
     * the native timer/delay contract rather than a number of poll calls. */
    if(detached_ms<USB_CDC_SOFT_DETACH_MS)
        return diagnostic_result(USB_CDC_E_DETACH_WAIT,USB_CDC_DIAG_DETACH_WAIT);
    if(!diagnostic_usb()) return diagnostic_result(USB_CDC_E_REVISION,USB_CDC_DIAG_REVISION);
    failure=(usb_diagnostic.control&1?USB_CDC_DIAG_ACTIVE:0u) |
            (usb_diagnostic.xcvr&0x80?USB_CDC_DIAG_PULLUP:0u);
    if(failure) return diagnostic_result(USB_CDC_E_BUSY,failure);
    W(MAIN,1);W(SIE,0xff);W(EP0_STAT,0x3f);W(FIFO_STAT,0x83);
    /* Auto negotiation needs reset/suspend event enables even when the CPU
     * interrupt itself is masked. MainIntEnb remains zero: we poll all events. */
    W(0x11,0xf0);
    /* VBUS can remain high across native MSC exit, soft detach, and attach.
     * Sample it after event clearing so a cleared event cannot hide a cable. */
    vbus=(unsigned char)(R(USB_STATUS)&0x80);
    W(USB_CTRL,vbus?0x41:1); /* Never run auto negotiation without VBUS. */
    W(XCVR,vbus?0x80:0x81);
    attached=1;
    if(!diagnostic_usb()) {
        drop_transport();return diagnostic_result(USB_CDC_E_REVISION,USB_CDC_DIAG_REVISION);
    }
    return diagnostic_result(0,0);
}
void usb_poll(void)
{
    unsigned stat,i,present,failure;
    if(!running || !attached) return;
    usb_diagnostic.poll_count++;
    failure=read_platform(&usb_diagnostic);
    if(failure) {
        usb_diagnostic.phase=USB_CDC_DIAG_POLL;usb_diagnostic.usb_valid=0;
        (void)diagnostic_result(USB_CDC_E_PLATFORM_STATE,failure);drop_transport();return;
    }
    if(!diagnostic_usb()) {
        usb_diagnostic.phase=USB_CDC_DIAG_POLL;
        (void)diagnostic_result(USB_CDC_E_REVISION,USB_CDC_DIAG_REVISION);drop_transport();return;
    }
    stat=R(SIE);
    present=R(USB_STATUS)&0x80;
    if(stat&0x20) {
        W(SIE,0x20);endpoints(0);W(ADDRESS,0);W(EP_ALL,0x20);
        W(USB_CTRL,present?0x41:1);
        ctrl_state=ST_IDLE;pending_config=0xff;pending_dtr=0xff;pending_interface=0xff;
    }
    if((stat&0x80) || present!=vbus) {
        W(SIE,0x80);
        vbus=(unsigned char)present;
        if(vbus) {
            W(0x11,0xf0);W(USB_CTRL,0x41);
            W(XCVR,0x80); /* OpMode=00, normal signaling. */
        }
        else {
            W(USB_CTRL,1); /* Manual VI-2-19: stop auto negotiation on unplug. */
            W(XCVR,0x81); /* OpMode=01, non-driving; see manual VI-2-52. */
            endpoints(0);ctrl_state=ST_IDLE;
        }
    }
    if(stat&0x10) W(SIE,0x10); /* Keep InSUSPEND until the bus leaves FS-J. */
    if(stat&0x40) {W(SIE,0x40);W(USB_CTRL,present?0x41:1);} /* Resume. */
    /* A subsequent SETUP can arrive after hardware completed status IN but
     * before the polling CPU handles it. Commit that acknowledged request
     * first; setup() must cancel only genuinely unacknowledged requests. */
    if(ctrl_state==ST_STATUS_IN && (R(EP0_STAT)&0x20)) control_status_ack();
    if(R(MAIN)&1) {setup();return;}
    stat=R(EP0_STAT);
    if(ctrl_state==ST_DATA_IN && ((R(FIFO_STAT)&0x80)||(stat&4))) {
        W(FIFO_STAT,0x80);W(EP0_STAT,4);W(EP0_CTRL,0);W(EP0_OUT,0x08);
        ctrl_state=ST_STATUS_OUT;
    } else if(ctrl_state==ST_STATUS_OUT && (stat&0x10)) {
        W(EP0_STAT,0x10);W(EP0_OUT,2);ctrl_state=ST_IDLE;
    } else if(ctrl_state==ST_LINE_OUT && (stat&0x10)) {
        W(EP0_STAT,0x10);W(EP0_OUT,2);W(JOIN_RD,0);W(CPU_ACCESS,1);
        if(get_pair(REMAIN_RD)!=7) {W(CPU_ACCESS,0);stall();return;}
        for(i=0;i<7;i++) ctrl_reply[i]=R(CPU_DATA);
        W(CPU_ACCESS,0);
        if(ctrl_reply[4]>2 || ctrl_reply[5]>4 || ctrl_reply[6]<5 || ctrl_reply[6]>8)
            stall();
        else {
            for(i=0;i<7;i++) line_coding[i]=ctrl_reply[i];
            status_in();
        }
    } else if(ctrl_state==ST_STATUS_IN && (stat&0x20)) control_status_ack();
}
int usb_receive(unsigned char *data, int capacity)
{
    unsigned count,i;
    if(!running || !configuration || !dtr || bulk_quiesced || !data || capacity<=0) return 0;
    W(JOIN_RD,2);W(CPU_ACCESS,1);
    count=MIN(get_pair(REMAIN_RD),(unsigned)capacity);count=MIN(count,64);
    for(i=0;i<count;i++) data[i]=R(CPU_DATA);
    W(CPU_ACCESS,0);W(JOIN_RD,0);
    if(count) {W(EPB_STAT,0x50);W(EPB_CTRL,0x20);}
    return (int)count;
}
int usb_send(const unsigned char *data, int length)
{
    unsigned count,i;
    if(!running || !configuration || !dtr || bulk_quiesced || !data || length<=0) return 0;
    /* Do not overwrite a pending short packet or emit empty idle packets. */
    if(R(EPA_CTRL)&1) return 0;
    if(tx_pending && !(R(EPA_STAT)&0x20)) return 0;
    if(tx_pending) {W(EPA_STAT,0x20);tx_pending=0;}
    W(EPA_CTRL,2);W(JOIN_WR,1);W(CPU_ACCESS,2);
    count=MIN(get_pair(REMAIN_WR),(unsigned)length);count=MIN(count,64);
    for(i=0;i<count;i++) W(CPU_DATA,data[i]);
    W(CPU_ACCESS,0);W(JOIN_WR,0);
    if(count) {
        W(EPA_STAT,0x20);W(EPA_CTRL,count<64?0x40:0);
        tx_pending=1;tx_need_zlp=(unsigned char)(count==64);
    }
    return (int)count;
}
int usb_tx_idle(void)
{
    if(!running || !configuration || !dtr || bulk_quiesced) return 0;
    if(tx_pending) {
        if(!(R(EPA_STAT)&0x20)) return 0;
        W(EPA_STAT,0x20);W(EPA_CTRL,2);tx_pending=0;
    }
    if(tx_need_zlp) {
        /* A full final packet needs an empty short packet to complete the
         * host CDC read transfer. No zero packets are sent while idle. */
        W(EPA_CTRL,0x40);tx_pending=1;tx_need_zlp=0;return 0;
    }
    return 1;
}
void usb_stop(void)
{
    usb_cdc_diagnostic state;
    if(!running) return;
    /* Cleanup must preserve diagnostic evidence and avoid invalid USB MMIO. */
    if((usb_diagnostic.phase==USB_CDC_DIAG_POLL && usb_diagnostic.result) ||
       (usb_diagnostic.failure_mask&USB_CDC_DIAG_REVISION) ||
       read_platform(&state)) {running=0;drop_transport();return;}
    W(XCVR,saved_xcvr&0x7f);W(USB_CTRL,0);
    endpoints(0);W(CPU_ACCESS,0);W(JOIN_RD,0);W(JOIN_WR,0);
    W(USB_CTRL,saved_usb_ctrl);W(XCVR,saved_xcvr);
    running=attached=0;ctrl_state=ST_IDLE;
}
int usb_is_configured(void) {return running && configuration;}
int usb_get_dtr(void) {return running && dtr && !bulk_quiesced;}
unsigned usb_get_session(void) {return session;}
