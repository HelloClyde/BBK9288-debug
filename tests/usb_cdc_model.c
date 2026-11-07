/* A register/FIFO model, not a USB electrical controller or physical test.
 * Build with both platform-gate and explicit timer-guard modes0/1.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define USB_CDC_TEST 1
#include "../device/usb_cdc.c"

static unsigned char mem[0x01000100], fifo[5][1024], descriptor_ram[1024];
static unsigned char descriptor_packet[64];
static unsigned descriptor_packet_bytes;
static unsigned counts[5], offsets[5], read_calls, write_calls;
static unsigned usb_read_calls,usb_write_calls;
static unsigned bulk_clear_calls;
static unsigned remain_latch[2],remain_high_read[2];
static unsigned char pending_hw_address=0xff;
static unsigned fifo_id(unsigned join)
{
    if(join&8) return 4;
    if(join&4) return 3;
    if(join&2) return 2;
    return (join&1)?1:0;
}
static void assert_usb_accessible(void)
{
    /* A failed bus/clock/IRQ gate must not read even USB's async registers.
     * The model fails immediately if diagnostic collection bypasses a gate. */
    assert((mem[0x48133]&0x11)==0x11 && !(mem[0x48132]&0x11));
    assert((mem[0x300f20]&0x30)==0x10 && !(mem[0x300f35]&0x80));
    assert(!(mem[0x402a6]&0x40));
    if(!D9288_USB_TIMER_GUARDED) assert(!(mem[0x40273]&0x40));
}
unsigned char usb_test_read(unsigned address)
{
    unsigned n,id,value;
    read_calls++;
    assert(address<sizeof(mem));
    if(address<BASE) return mem[address];
    assert_usb_accessible();usb_read_calls++;
    n=address-BASE;
    /* These locations are reserved in the independent hardware register map.
     * Earlier code accidentally used 3C for EP0MaxSize and 44/48 for EPb/c. */
    assert(!(n>=0x3c && n<=0x3e));
    assert(!(n>=0x44 && n<=0x4f));
    if(n>=REMAIN_RD && n<=REMAIN_WR+1) {
        unsigned pair=n>=REMAIN_WR;
        if(!(n&1)) {
            id=fifo_id(mem[BASE+(pair?JOIN_WR:JOIN_RD)]);
            value=pair?(id?256:64)-counts[id]:counts[id];
            remain_latch[pair]=value;remain_high_read[pair]=1;
            return (unsigned char)(value>>8);
        }
        /* The low byte is meaningful only after the high-byte latch read. */
        assert(remain_high_read[pair]);remain_high_read[pair]=0;
        return (unsigned char)remain_latch[pair];
    }
    if(n==CPU_DATA) {
        assert(mem[BASE+CPU_ACCESS]&1);
        id=fifo_id(mem[BASE+JOIN_RD]);assert(counts[id]);counts[id]--;
        return fifo[id][offsets[id]++];
    }
    return mem[address];
}
void usb_test_write(unsigned address, unsigned char value)
{
    unsigned n,id,i;
    write_calls++;
    assert(address<sizeof(mem));
    if(address<BASE) {mem[address]=value;return;}
    assert_usb_accessible();usb_write_calls++;
    n=address-BASE;
    assert(!(n>=0x3c && n<=0x3e));
    assert(!(n>=0x44 && n<=0x4f));
    if(n==JOIN_RD) assert(!(mem[BASE+CPU_ACCESS]&1));
    if(n==JOIN_WR) assert(!(mem[BASE+CPU_ACCESS]&2));
    if(n==CPU_ACCESS && value) assert(!mem[BASE+0x91]); /* No DMA/PIO overlap. */
    if(n==MAIN || n==SIE || n==3 || n==FIFO_STAT || (n>=7 && n<12)) {
        mem[address]&=(unsigned char)~value;return;
    }
    if(n==EP_CLEAR || n==EP_ALL) {
        if(n==EP_CLEAR && (value&3u)) bulk_clear_calls++;
        if(n==EP_ALL) {
            if(value&0x20) for(i=0;i<5;i++) counts[i]=offsets[i]=0;
            if(value&1) counts[0]=offsets[0]=0;
        } else for(i=0;i<4;i++) if(value&(1u<<i)) counts[i+1]=offsets[i+1]=0;
        return;
    }
    if(n==CPU_DATA) {
        assert(mem[BASE+CPU_ACCESS]&2);
        id=fifo_id(mem[BASE+JOIN_WR]);assert(counts[id]<sizeof(fifo[id]));
        fifo[id][counts[id]++]=value;return;
    }
    if(n==DESC_DOOR) {
        i=((unsigned)mem[BASE+DESC_ADDR]<<8)|mem[BASE+DESC_ADDR+1];
        assert(i<sizeof(descriptor_ram));descriptor_ram[i++]=value;
        mem[BASE+DESC_ADDR]=(unsigned char)(i>>8);
        mem[BASE+DESC_ADDR+1]=(unsigned char)i;return;
    }
    if(n==ADDRESS) pending_hw_address=(value&0x80)?mem[BASE+0x32]&127:0xff;
    if(n==EP0_IN || n==EP0_OUT || (n>=0x40 && n<=0x43)) {
        unsigned toggle=mem[address]&0x10;
        if(value&8) toggle=0x10;
        if(value&4) toggle=0;
        value=(unsigned char)((value&~0x1c)|toggle);
    }
    mem[address]=value;
    if(n==EP0_CTRL && (value&1u)) {
        unsigned remaining=((unsigned)mem[BASE+DESC_SIZE]<<8)|mem[BASE+DESC_SIZE+1];
        assert(value&0x80u);
        /* VI-2-14: ReplyDescriptor itself enables a pending short packet.
         * A later full-byte write to EP0_IN can accidentally clear it. */
        if(remaining && remaining<mem[BASE+EP0_MAX]) mem[BASE+EP0_IN]|=0x40;
    }
}
void usb_test_clear_bit(unsigned address,unsigned bit)
{
    assert(address==BASE+EP0_IN && bit==1u);
    assert_usb_accessible();read_calls++;write_calls++;usb_read_calls++;usb_write_calls++;
    /* Atomic peripheral bit instruction: all other hardware state survives. */
    mem[address]&=(unsigned char)~(1u<<bit);
}
static void good_platform(void)
{
    memset(mem,0,sizeof(mem));
    mem[0x00048133]=0x11;mem[0x00300f20]=0x10;
    mem[BASE+0x20]=0x12;mem[BASE+XCVR]=1;
    mem[BASE+USB_STATUS]=0x40;
    mem[BASE+0x3f]=8; /* Real reset value of EP0MaxSize; not the descriptor's 64. */
    pending_hw_address=0xff;
    descriptor_packet_bytes=0;
}
static void request(unsigned type,unsigned op,unsigned value,unsigned index,unsigned length)
{
    unsigned char p[8]={(unsigned char)type,(unsigned char)op,(unsigned char)value,
        (unsigned char)(value>>8),(unsigned char)index,(unsigned char)(index>>8),
        (unsigned char)length,(unsigned char)(length>>8)};
    /* A non-driving transceiver cannot receive any host request. */
    assert((mem[BASE+USB_STATUS]&0x80) && (mem[BASE+XCVR]&3)==0);
    /* SETUP aborts a previous descriptor reply and initializes DATA1/NAK. */
    mem[BASE+EP0_CTRL]&=(unsigned char)~1u;
    mem[BASE+EP0_IN]=(mem[BASE+EP0_IN]&~1u)|0x12u;
    mem[BASE+EP0_OUT]=(mem[BASE+EP0_OUT]&~1u)|0x12u;
    memcpy(mem+BASE+0x30,p,8);mem[BASE+MAIN]|=1;usb_poll();
}
static void status_ack_latched(void)
{
    /* An IN status ACK requires an enabled zero-length DATA1 packet. */
    assert(ctrl_state==ST_STATUS_IN && (mem[BASE+EP0_CTRL]&0x80));
    assert(!(mem[BASE+EP0_IN]&3) && (mem[BASE+EP0_IN]&0x40) && !counts[0]);
    if(pending_hw_address!=0xff) {
        mem[BASE+ADDRESS]=pending_hw_address;pending_hw_address=0xff;
        mem[BASE+SIE]|=1; /* AutoSetAddress applies only after the status ACK. */
    }
    mem[BASE+EP0_IN]=(mem[BASE+EP0_IN]&~0x40u)|2u;
    mem[BASE+EP0_STAT]|=0x20;
}
static void status_ack(void) {status_ack_latched();usb_poll();}
static int bulk_out_token(const unsigned char *data,unsigned bytes)
{
    assert(bytes<=64u);
    /* The SIE can finish status IN and receive a subsequent OUT before the
     * polling CPU observes EP0's ACK. An accepted OUT must not then vanish. */
    if(!(mem[BASE+0x56]&0x20u) || (mem[BASE+EPB_CTRL]&3u)) return 0;
    assert(counts[2]+bytes<=256u);memcpy(fifo[2]+counts[2],data,bytes);counts[2]+=bytes;
    mem[BASE+EPB_CTRL]^=0x10u;mem[BASE+EPB_STAT]|=0x50u;
    return 1;
}
static void in_ack(void)
{counts[1]=offsets[1]=0;mem[BASE+EPA_STAT]|=0x20;mem[BASE+EPA_CTRL]=(mem[BASE+EPA_CTRL]&~0x40)^0x10;}
static int descriptor_in_token(void)
{
    unsigned remaining=((unsigned)mem[BASE+DESC_SIZE]<<8)|mem[BASE+DESC_SIZE+1];
    unsigned address=((unsigned)mem[BASE+DESC_ADDR]<<8)|mem[BASE+DESC_ADDR+1];
    unsigned maximum=mem[BASE+EP0_MAX];
    unsigned transferred=remaining<maximum?remaining:maximum;
    descriptor_packet_bytes=0;
    assert(maximum && maximum<=sizeof(descriptor_packet));
    /* VI-2-12: no short enable and fewer than MAX bytes means NAK. A NAK
     * neither consumes data nor completes the descriptor operation. */
    if(!(mem[BASE+EP0_CTRL]&0x80) || (mem[BASE+EP0_IN]&3) ||
       !(mem[BASE+EP0_CTRL]&1) || !remaining ||
       (transferred<maximum && !(mem[BASE+EP0_IN]&0x40))) {
        mem[BASE+EP0_STAT]|=8;usb_poll();return 0;
    }
    assert(address+transferred<=sizeof(descriptor_ram));
    memcpy(descriptor_packet,descriptor_ram+address,transferred);
    descriptor_packet_bytes=transferred;
    remaining-=transferred;address+=transferred;
    mem[BASE+DESC_SIZE]=(unsigned char)(remaining>>8);mem[BASE+DESC_SIZE+1]=(unsigned char)remaining;
    mem[BASE+DESC_ADDR]=(unsigned char)(address>>8);mem[BASE+DESC_ADDR+1]=(unsigned char)address;
    mem[BASE+EP0_IN]^=0x10;
    if(transferred<maximum) {
        mem[BASE+EP0_IN]&=(unsigned char)~0x40u;
        mem[BASE+EP0_IN]|=2; /* A completed short IN automatically forces NAK. */
    }
    if(!remaining) {
        mem[BASE+EP0_CTRL]&=(unsigned char)~1u;
        mem[BASE+FIFO_STAT]|=0x80;mem[BASE+EP0_STAT]|=0x20;
    } else if(remaining<maximum) mem[BASE+EP0_IN]|=0x40;
    usb_poll();
    return 1;
}
static void descriptor_packet_ack(void)
{
    assert(descriptor_in_token());
}
static void complete_reply(void)
{
    unsigned packets=0;
    while(ctrl_state==ST_DATA_IN) {assert(packets++<3u);descriptor_packet_ack();}
    assert(ctrl_state==ST_STATUS_OUT && !mem[BASE+EP0_CTRL]);
    assert(!(mem[BASE+EP0_OUT]&2) && (mem[BASE+EP0_OUT]&0x10));
    mem[BASE+EP0_STAT]|=0x10;usb_poll();
    assert(ctrl_state==ST_IDLE);
}
int main(void)
{
    unsigned before,i,toggle,usb_reads,usb_writes; unsigned char out[100],packet[100];
    usb_cdc_diagnostic saved_diagnostic;
    assert(sizeof(usb_cdc_diagnostic)==108u);
    assert(EP0_MAX==0x3f && EPA_CTRL==0x40 && EPB_CTRL==0x41 && EPC_CTRL==0x42);
    good_platform();
    if(!D9288_USB_PLATFORM_VERIFIED) {
        assert(usb_start()==USB_CDC_E_PLATFORM_UNVERIFIED);
        assert(usb_diagnostic.version==1 && usb_diagnostic.phase==USB_CDC_DIAG_START);
        assert(usb_diagnostic.result==(unsigned)USB_CDC_E_PLATFORM_UNVERIFIED);
        assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_UNVERIFIED && !usb_diagnostic.usb_valid);
        assert(!read_calls && !write_calls);
        assert(usb_attach(USB_CDC_SOFT_DETACH_MS)==USB_CDC_E_PLATFORM_UNVERIFIED);
        usb_poll();usb_stop();assert(!read_calls && !write_calls);
        puts("USB disabled gate: PASS (zero MMIO)");return 0;
    }
    mem[0x00300f20]=0;assert(usb_start()==USB_CDC_E_PLATFORM_STATE);
    assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_CLOCK_MISSING);
    assert(usb_diagnostic.misc_f20==0 && usb_diagnostic.bcu_hi==0x11 && !usb_diagnostic.usb_valid);
    assert(usb_diagnostic.result==(unsigned)USB_CDC_E_PLATFORM_STATE);
    assert(!write_calls);good_platform();
    mem[0x00300f20]=0x27;assert(usb_start()==USB_CDC_E_PLATFORM_STATE);
    assert(usb_diagnostic.failure_mask==(USB_CDC_DIAG_CLOCK_MISSING|USB_CDC_DIAG_SNOOZE));
    assert(usb_diagnostic.misc_f20==0x27 && !usb_diagnostic.usb_valid);
    assert(!write_calls);good_platform();
    mem[0x00048133]=0;assert(usb_start()==USB_CDC_E_PLATFORM_STATE);
    assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_BCU_HI);
    assert(!write_calls);good_platform();
    mem[0x00048132]=0x11;assert(usb_start()==USB_CDC_E_PLATFORM_STATE);
    assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_BCU_LO);
    assert(!write_calls);good_platform();
    mem[0x00300f35]=0x80;assert(usb_start()==USB_CDC_E_PLATFORM_STATE);
    assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_MODULE_CLOCK);
    assert(!write_calls);good_platform();
    mem[0x000402a6]=0x40;assert(usb_start()==USB_CDC_E_PLATFORM_STATE);
    assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_USB_IRQ);
    assert(usb_diagnostic.usb_irq==0x40 && !usb_diagnostic.usb_valid);
    assert(!write_calls);good_platform();
    if(!D9288_USB_TIMER_GUARDED) {
        mem[0x00040273]=0x40;assert(usb_start()==USB_CDC_E_PLATFORM_STATE);
        assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_TIMER_IRQ);
        assert(!write_calls);good_platform();
    }
    assert(!usb_read_calls && !usb_write_calls);
    mem[BASE+0x20]=0x11;assert(usb_start()==USB_CDC_E_REVISION);
    assert(usb_read_calls==1 && !usb_write_calls);
    assert(usb_diagnostic.revision==0x11 && !usb_diagnostic.usb_valid);
    assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_REVISION);good_platform();
    mem[BASE+USB_CTRL]=1;assert(usb_start()==USB_CDC_E_BUSY);
    assert(usb_diagnostic.usb_valid && usb_diagnostic.control==1);
    assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_ACTIVE);
    assert(!write_calls);good_platform();
    mem[BASE+XCVR]=0x80;assert(usb_start()==USB_CDC_E_BUSY);
    assert(usb_diagnostic.usb_valid && usb_diagnostic.xcvr==0x80);
    assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_PULLUP);
    assert(!write_calls);good_platform();
    /* Native GUI key reads re-enable this bit. The guarded build delegates
     * timer ownership to the platform's verified heap IRQ vector instead. */
    if(D9288_USB_TIMER_GUARDED) mem[0x00040273]=0x40;
    mem[BASE+USB_STATUS]|=0x80; /* USB power stays present after native MSC exit. */
    assert(!usb_start());
    assert(usb_diagnostic.phase==USB_CDC_DIAG_START && !usb_diagnostic.result);
    assert(usb_diagnostic.usb_valid && usb_diagnostic.ep0_max==64);
    assert(!usb_diagnostic.control && usb_diagnostic.xcvr==1);
    assert(mem[BASE+0x3f]==64);
    assert(!(mem[BASE+USB_CTRL]&1) && !(mem[BASE+XCVR]&0x80));
    before=write_calls;usb_reads=read_calls;
    mem[BASE+SIE]|=0xe0; /* A pending event must not bypass the detach interval. */
    usb_poll();assert(write_calls==before && !(mem[BASE+XCVR]&0x80));
    assert(read_calls==usb_reads);
    assert(usb_attach(0)==USB_CDC_E_DETACH_WAIT);
    assert(usb_attach(USB_CDC_SOFT_DETACH_MS-1)==USB_CDC_E_DETACH_WAIT);
    assert(usb_diagnostic.phase==USB_CDC_DIAG_ATTACH);
    assert(usb_diagnostic.result==(unsigned)USB_CDC_E_DETACH_WAIT);
    assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_DETACH_WAIT && !usb_diagnostic.usb_valid);
    assert(write_calls==before && !(mem[BASE+XCVR]&0x80));
    mem[0x000402a6]=0x40;
    assert(usb_attach(USB_CDC_SOFT_DETACH_MS)==USB_CDC_E_PLATFORM_STATE);
    assert(write_calls==before && !(mem[BASE+XCVR]&0x80));mem[0x000402a6]=0;
    mem[0x00300f20]=0;
    assert(usb_attach(USB_CDC_SOFT_DETACH_MS)==USB_CDC_E_PLATFORM_STATE);
    assert(write_calls==before);mem[0x00300f20]=0x10;
    mem[0x00048133]=0;
    assert(usb_attach(USB_CDC_SOFT_DETACH_MS)==USB_CDC_E_PLATFORM_STATE);
    assert(write_calls==before);mem[0x00048133]=0x11;
    mem[BASE+USB_CTRL]=1;
    assert(usb_attach(USB_CDC_SOFT_DETACH_MS)==USB_CDC_E_BUSY);
    assert(write_calls==before);mem[BASE+USB_CTRL]=0;
    assert(!usb_attach(USB_CDC_SOFT_DETACH_MS));
    assert(usb_diagnostic.phase==USB_CDC_DIAG_ATTACH && !usb_diagnostic.result);
    assert(usb_diagnostic.usb_valid && usb_diagnostic.control==0x41 && usb_diagnostic.xcvr==0x80);
    assert(mem[BASE+USB_CTRL]==0x41 && (mem[BASE+0x11]&0x30)==0x30);
    assert(!mem[BASE+0x10]); /* SIE enables do not enable a CPU interrupt. */
    assert(mem[BASE+XCVR]==0x80);
    assert(usb_attach(USB_CDC_SOFT_DETACH_MS)==USB_CDC_E_BUSY);
    /* A VBUS event may have been cleared during initialization. The level
     * alone must still take the transceiver out of non-driving mode. */
    mem[BASE+USB_STATUS]|=0x80;usb_poll();
    assert(mem[BASE+XCVR]==0x80);
    assert(usb_start()==USB_CDC_E_BUSY);
    request(0x80,6,0x100,0,0);
    assert(usb_diagnostic.setup_count==1 && usb_diagnostic.poll_count>0);
    assert(ctrl_state==ST_STATUS_IN && (mem[BASE+EP0_IN]&0x40));status_ack();
    request(0x80,6,0x100,0,8);
    assert(mem[BASE+DESC_SIZE+1]==8);
    /* The initial short descriptor exposes device-level ACM subclass02,
     * protocol01 so Windows can bind its inbox usbser.inf without an INF. */
    assert(!memcmp(descriptor_ram+64,"\x12\x01\x00\x02\x02\x02\x01\x40",8));
    complete_reply();
    assert(descriptor_packet_bytes==8);
    assert(!memcmp(descriptor_packet,"\x12\x01\x00\x02\x02\x02\x01\x40",8));
    /* Reproduce the old arm-then-W08 corruption: the hardware short enable
     * disappears and an IN token must NAK without advancing address/size. */
    request(0x80,6,0x100,0,64);
    assert(mem[BASE+DESC_SIZE+1]==18 && (mem[BASE+EP0_IN]&0x40));
    usb_test_write(BASE+EP0_IN,8);
    assert(!descriptor_in_token() && !descriptor_packet_bytes);
    assert(mem[BASE+DESC_ADDR+1]==64 && mem[BASE+DESC_SIZE+1]==18);
    assert(ctrl_state==ST_DATA_IN && (mem[BASE+EP0_STAT]&8));
    request(0x80,6,0x100,0,64);complete_reply();
    assert(descriptor_packet_bytes==18);
    assert(!memcmp(descriptor_packet,device_descriptor,18));
    request(0x80,6,0x200,0,255);
    assert(mem[BASE+DESC_SIZE+1]==67);
    assert(descriptor_ram[64+2]==67 && descriptor_ram[64+4]==2);
    assert(descriptor_ram[64+39]==0x83);
    descriptor_packet_ack();assert(mem[BASE+DESC_SIZE+1]==3 && ctrl_state==ST_DATA_IN);
    assert(descriptor_packet_bytes==64 && (mem[BASE+EP0_IN]&0x40));
    descriptor_packet_ack();assert(ctrl_state==ST_STATUS_OUT && descriptor_packet_bytes==3);
    assert(!memcmp(descriptor_packet,config_descriptor+64,3));
    assert(!mem[BASE+EP0_CTRL] && !(mem[BASE+EP0_OUT]&2));
    mem[BASE+EP0_STAT]|=0x10;usb_poll();assert(ctrl_state==ST_IDLE);
    request(0,5,9,0,0);assert(mem[BASE+ADDRESS]&0x80);
    assert((mem[BASE+ADDRESS]&127)==0);status_ack();assert((mem[BASE+ADDRESS]&127)==9);
    request(0,5,17,0,0);assert(mem[BASE+ADDRESS]&0x80);
    request(0x80,6,0x100,0,8);
    assert(!(mem[BASE+ADDRESS]&0x80));complete_reply();
    request(0,9,0,0,0);status_ack();assert(mem[BASE+ADDRESS]==9);
    request(0,9,1,0,0);assert(!usb_is_configured());status_ack_latched();
    /* A new control SETUP must not erase an already hardware-ACKed config. */
    request(0x80,8,0,0,1);assert(usb_is_configured());
    assert(descriptor_ram[64]==1u);complete_reply();
    assert(usb_is_configured());assert(mem[BASE+0x52]==0xa1);
    assert(mem[BASE+0x56]==0x22 && mem[BASE+0x5a]==0xa3);
    assert(mem[BASE+EPB_CTRL]&2u);assert(!bulk_out_token(out,1u));
    before=usb_get_session();request(0x21,0x22,1,0,0);
    assert(!usb_get_dtr());status_ack();assert(usb_get_dtr());
    assert(usb_get_session()==before+1);
    for(i=0;i<100;i++) packet[i]=(unsigned char)i;
    assert(usb_send(packet,100)==64);assert(counts[1]==64);
    assert(!memcmp(fifo[1],packet,64));assert(!usb_tx_idle());
    assert(usb_send(packet+64,36)==0);in_ack();
    assert(usb_send(packet+64,36)==36);assert(counts[1]==36);
    assert(!usb_tx_idle());in_ack();assert(usb_tx_idle());
    /* A full packet in the middle of a larger response must not append a ZLP. */
    assert(usb_send(packet,64)==64);in_ack();assert(usb_send(packet,64)==64);
    assert(counts[1]==64 && !(mem[BASE+EPA_CTRL]&0x40));in_ack();
    assert(usb_send(packet,36)==36);in_ack();assert(usb_tx_idle());
    assert(usb_send(packet,64)==64);in_ack();assert(!usb_tx_idle());
    assert(!counts[1] && (mem[BASE+EPA_CTRL]&0x40));
    assert(!usb_tx_idle());in_ack();assert(usb_tx_idle());
    memcpy(fifo[2],packet,100);counts[2]=100;offsets[2]=0;
    assert(usb_receive(out,100)==64 && counts[2]==36);
    assert(usb_receive(out+64,36)==36 && !counts[2]);
    assert(!memcmp(out,packet,100));
    request(0x21,0x20,0,0,7);
    memcpy(fifo[0],"\x80\x25\x00\x00\x00\x00\x08",7);
    counts[0]=7;offsets[0]=0;mem[BASE+EP0_STAT]|=0x10;usb_poll();status_ack();
    request(0xa1,0x21,0,0,7);assert(mem[BASE+DESC_SIZE+1]==7);
    assert(!memcmp(descriptor_ram+64,"\x80\x25\x00\x00\x00\x00\x08",7));
    complete_reply();
    request(0x21,0x20,0,0,7);
    memcpy(fifo[0],"\x80\x25\x00\x00\x00\x00\x01",7);
    counts[0]=7;offsets[0]=0;mem[BASE+EP0_STAT]|=0x10;usb_poll();
    assert(mem[BASE+EP0_IN]&1); /* Unsupported data bits: retain old line coding. */
    request(0xa1,0x21,0,0,7);assert(!memcmp(descriptor_ram+64,"\x80\x25\x00\x00\x00\x00\x08",7));complete_reply();
    request(0x21,0x20,0,0,7);counts[0]=0;mem[BASE+EP0_STAT]|=0x10;usb_poll();
    assert(mem[BASE+EP0_IN]&1);
    request(0x80,6,0x600,0,10);assert(mem[BASE+EP0_IN]&1);
    request(2,3,0,0x81,0);status_ack();assert(mem[BASE+EPA_CTRL]&1);
    request(2,1,0,0x81,0);status_ack();assert(!(mem[BASE+EPA_CTRL]&1));
    assert(!(mem[BASE+EPA_CTRL]&0x10));
    /* Alternate setting zero still resets the affected interface's DATA0. */
    mem[BASE+EPA_CTRL]|=0x10;mem[BASE+EPB_CTRL]|=0x10;mem[BASE+EPC_CTRL]|=0x10;
    before=usb_get_session();request(1,11,0,0,0);assert(mem[BASE+EPC_CTRL]&0x10);status_ack();
    assert(!(mem[BASE+EPC_CTRL]&0x10) && (mem[BASE+EPA_CTRL]&0x10));
    assert(usb_get_session()==before && usb_get_dtr());
    request(1,11,0,1,0);status_ack();
    assert(!(mem[BASE+EPA_CTRL]&0x10) && !(mem[BASE+EPB_CTRL]&0x10));
    assert(usb_get_session()==before+1 && usb_get_dtr());
    mem[BASE+EPA_CTRL]|=0x10;mem[BASE+EPB_CTRL]|=0x10;
    toggle=mem[BASE+EPA_CTRL]&0x10;
    memcpy(fifo[2],packet,20);counts[2]=20;before=usb_get_session();
    memcpy(fifo[1],packet,12);counts[1]=12;tx_pending=tx_need_zlp=1;
    request(0x21,0x22,0,0,0);
    assert(usb_get_session()==before+1u && !usb_get_dtr() && bulk_quiesced);
    assert(!counts[1] && !counts[2] && !tx_pending && !tx_need_zlp);
    usb_reads=usb_read_calls;usb_writes=usb_write_calls;
    assert(!usb_receive(out,sizeof(out)) && !usb_send(packet,16) && !usb_tx_idle());
    assert(usb_reads==usb_read_calls && usb_writes==usb_write_calls);
    status_ack();assert(!usb_get_dtr());
    assert(!counts[2] && usb_get_session()==before+1);
    assert((mem[BASE+EPA_CTRL]&0x10)==toggle && (mem[BASE+EPB_CTRL]&0x10));
    assert(!bulk_out_token(packet,16u));
    request(2,1,0,2,0);status_ack();assert(mem[BASE+EPB_CTRL]&2u);
    request(1,11,0,1,0);status_ack();assert(mem[BASE+EPB_CTRL]&2u);
    /* Windows may send PING immediately after hardware ACK of DTR rising,
     * before the next device CPU poll. A NAK can be retried; an ACKed packet
     * may never be discarded by delayed DTR handling. */
    request(0x21,0x22,1,0,0);status_ack_latched();
    {
        unsigned clears=bulk_clear_calls;
        int accepted=bulk_out_token(packet,16u);
        usb_poll();assert(usb_get_dtr());
        assert(bulk_clear_calls==clears);
        if(!accepted) assert(bulk_out_token(packet,16u));
        if(counts[2]!=16u) fprintf(stderr,"DTR ACK/OUT race: OUT ACKed=%d, RX bytes after delayed poll=%u\n",accepted,counts[2]);
        assert(usb_receive(out,16)==16 && !memcmp(out,packet,16u));
    }
    /* A repeated DTR1 (including an RTS-only change) also isolates stale
     * queued TX/RX when a reopen did not deliver a DTR0 notification. */
    memcpy(fifo[2],packet,11);counts[2]=11;offsets[2]=0;
    memcpy(fifo[1],packet,9);counts[1]=9;tx_pending=tx_need_zlp=1;
    before=usb_get_session();request(0x21,0x22,3,0,0);
    assert(usb_get_session()==before+1u && !usb_get_dtr() && dtr);
    assert(!counts[1] && !counts[2] && !tx_pending && !tx_need_zlp);
    assert(!usb_receive(out,16) && !usb_send(packet,16) && !usb_tx_idle());
    status_ack();assert(usb_get_dtr());
    assert(bulk_out_token(packet,16u));assert(usb_receive(out,16)==16);
    /* A new SETUP can either abort unacknowledged DTR1 or follow its ACK.
     * Preserve the old level only in the first case. */
    request(0x21,0x22,0,0,0);status_ack();assert(!usb_get_dtr());
    before=usb_get_session();request(0x21,0x22,1,0,0);
    request(0xa1,0x21,0,0,7);complete_reply();
    assert(!usb_get_dtr() && !bulk_quiesced && (mem[BASE+EPB_CTRL]&2u));
    assert(usb_get_session()==before+1u);
    request(0x21,0x22,1,0,0);status_ack_latched();
    request(0xa1,0x21,0,0,7);complete_reply();
    assert(usb_get_dtr() && !bulk_quiesced && !(mem[BASE+EPB_CTRL]&2u));
    assert(bulk_out_token(packet,16u));assert(usb_receive(out,16)==16);
    mem[BASE+USB_CTRL]|=0x20;mem[BASE+SIE]|=0x10;usb_poll();
    assert(mem[BASE+USB_CTRL]&0x20);
    mem[BASE+SIE]|=0x40;usb_poll();assert(mem[BASE+USB_CTRL]==0x41);
    mem[BASE+SIE]|=0x20;usb_poll();assert(!usb_is_configured());
    mem[BASE+USB_STATUS]&=0x7f;mem[BASE+SIE]|=0x80;usb_poll();
    assert(mem[BASE+XCVR]==0x81 && mem[BASE+USB_CTRL]==1);
    mem[BASE+SIE]|=0x60;usb_poll();assert(mem[BASE+USB_CTRL]==1);
    saved_diagnostic=usb_diagnostic;
    usb_stop();assert(!running);assert(!(mem[BASE+XCVR]&0x80));
    assert(!memcmp((const void*)&usb_diagnostic,&saved_diagnostic,sizeof(saved_diagnostic)));
    /* An ordinary disconnected start still needs the explicit attach phase. */
    good_platform();assert(!usb_start());assert(!(mem[BASE+XCVR]&0x80));
    assert(!usb_attach(USB_CDC_SOFT_DETACH_MS));
    assert(mem[BASE+XCVR]==0x81 && mem[BASE+USB_CTRL]==1);
    mem[BASE+USB_STATUS]|=0x80;usb_poll();
    assert(mem[BASE+XCVR]==0x80 && mem[BASE+USB_CTRL]==0x41);
    usb_stop();
    /* Stop during the delay cancels attachment and leaves D+ disconnected. */
    good_platform();mem[BASE+USB_STATUS]|=0x80;assert(!usb_start());usb_stop();
    before=write_calls;assert(usb_attach(USB_CDC_SOFT_DETACH_MS)==USB_CDC_E_BUSY);
    assert(write_calls==before && !(mem[BASE+XCVR]&0x80));
    /* An attach-time clock failure survives cleanup without USB MMIO. */
    good_platform();assert(!usb_start());mem[0x300f20]=0x27;
    usb_reads=usb_read_calls;usb_writes=usb_write_calls;
    assert(usb_attach(USB_CDC_SOFT_DETACH_MS)==USB_CDC_E_PLATFORM_STATE);
    assert(usb_diagnostic.phase==USB_CDC_DIAG_ATTACH && usb_diagnostic.misc_f20==0x27);
    saved_diagnostic=usb_diagnostic;usb_stop();
    assert(usb_read_calls==usb_reads && usb_write_calls==usb_writes);
    assert(!memcmp((const void*)&usb_diagnostic,&saved_diagnostic,sizeof(saved_diagnostic)));
    /* Runtime gate loss captures its cause and prevents later transport I/O. */
    good_platform();mem[BASE+USB_STATUS]|=0x80;assert(!usb_start());
    assert(!usb_attach(USB_CDC_SOFT_DETACH_MS));
    request(0,9,1,0,0);status_ack();request(0x21,0x22,1,0,0);status_ack();
    assert(usb_get_dtr());mem[0x402a6]=0x40;
    usb_reads=usb_read_calls;usb_writes=usb_write_calls;usb_poll();
    assert(usb_diagnostic.phase==USB_CDC_DIAG_POLL && !usb_diagnostic.usb_valid);
    assert(usb_diagnostic.failure_mask==USB_CDC_DIAG_USB_IRQ);
    assert(!usb_get_dtr() && !usb_is_configured());
    assert(!usb_send(packet,64) && !usb_receive(out,64) && !usb_tx_idle());
    saved_diagnostic=usb_diagnostic;usb_stop();
    assert(usb_read_calls==usb_reads && usb_write_calls==usb_writes);
    assert(!memcmp((const void*)&usb_diagnostic,&saved_diagnostic,sizeof(saved_diagnostic)));
    /* A revision mismatch allows only the revision read, then even cleanup
     * must avoid the controller despite otherwise valid clock/bus gates. */
    good_platform();mem[BASE+USB_STATUS]|=0x80;assert(!usb_start());
    assert(!usb_attach(USB_CDC_SOFT_DETACH_MS));
    request(0,9,1,0,0);status_ack();request(0x21,0x22,1,0,0);status_ack();
    mem[BASE+0x20]=0x11;usb_reads=usb_read_calls;usb_writes=usb_write_calls;
    usb_poll();
    assert(usb_read_calls==usb_reads+1u && usb_write_calls==usb_writes);
    assert(usb_diagnostic.phase==USB_CDC_DIAG_POLL && !usb_diagnostic.usb_valid);
    assert(usb_diagnostic.revision==0x11 && usb_diagnostic.failure_mask==USB_CDC_DIAG_REVISION);
    assert(usb_diagnostic.result==(unsigned)USB_CDC_E_REVISION);
    assert(!usb_send(packet,64) && !usb_receive(out,64) && !usb_tx_idle());
    saved_diagnostic=usb_diagnostic;usb_stop();
    assert(usb_read_calls==usb_reads+1u && usb_write_calls==usb_writes);
    assert(!memcmp((const void*)&usb_diagnostic,&saved_diagnostic,sizeof(saved_diagnostic)));
    puts("USB model: PASS (descriptors, deferred configuration, DTR, bulk, ZLP, line coding, halt, reset)");
    return 0;
}
