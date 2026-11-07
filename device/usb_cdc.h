#ifndef D9288_USB_CDC_H
#define D9288_USB_CDC_H

/* Experimental S1C33L05 USB 1.1 PIO CDC transport.
 * All calls belong to the same foreground/worker context. No ISR owns it.
 * The default build fails closed until a platform owner explicitly enables
 * it. That compile gate is separate from successful physical enumeration.
 * D9288_USB_TIMER_GUARDED=1 additionally delegates Timer3B protection to the
 * platform's owned IRQ-vector guard, which must be checked before usb_start.
 * Its default0 requires Timer3B's native USB interrupt-enable bit to be clear.
 */
#define USB_CDC_E_PLATFORM_UNVERIFIED (-10)
#define USB_CDC_E_BUSY                (-11)
#define USB_CDC_E_PLATFORM_STATE      (-12)
#define USB_CDC_E_REVISION            (-13)
#define USB_CDC_E_DETACH_WAIT         (-14)
#define USB_CDC_SOFT_DETACH_MS        250u

#define USB_CDC_DIAG_VERSION 1u
#define USB_CDC_DIAG_START   1u
#define USB_CDC_DIAG_ATTACH  2u
#define USB_CDC_DIAG_POLL    3u
#define USB_CDC_DIAG_BCU_HI          0x0001u
#define USB_CDC_DIAG_BCU_LO          0x0002u
#define USB_CDC_DIAG_CLOCK_MISSING   0x0004u
#define USB_CDC_DIAG_SNOOZE          0x0008u
#define USB_CDC_DIAG_MODULE_CLOCK    0x0010u
#define USB_CDC_DIAG_USB_IRQ         0x0020u
#define USB_CDC_DIAG_TIMER_IRQ       0x0040u
#define USB_CDC_DIAG_REVISION        0x0080u
#define USB_CDC_DIAG_ACTIVE          0x0100u
#define USB_CDC_DIAG_PULLUP          0x0200u
#define USB_CDC_DIAG_DETACH_WAIT     0x0400u
#define USB_CDC_DIAG_BUSY            0x0800u
#define USB_CDC_DIAG_UNVERIFIED      0x1000u

/* All27 fields are32-bit unsigned words, in this exact order (108 bytes).
 * result holds the bit pattern of the signed API result. usb_valid is1 only
 * for a complete USB snapshot after valid bus/clock gates and revision12.
 * Poll failures use phase3; healthy polls retain the last start/attach result.
 * stop preserves this record, including failure details and the last snapshot.
 */
typedef struct usb_cdc_diagnostic {
    unsigned version,phase,result,failure_mask;
    unsigned bcu_hi,bcu_lo,misc_f20,clock_f35,usb_irq,timer_irq;
    unsigned usb_valid,revision,control,status,xcvr,main_status,sie_status;
    unsigned sie_enable,test,ep0_status,ep0_control,ep0_in,ep0_out,ep0_max,address;
    unsigned setup_count,poll_count;
} usb_cdc_diagnostic;
extern volatile usb_cdc_diagnostic usb_diagnostic;

/* Prepare with D+ pullup disabled, even if cable power/VBUS remains present.
 * Requires a previously claimed inactive controller with native paths owned. */
int usb_start(void);
/* After a verified ROS delay and a fresh platform-owner check, connect.
 * detached_ms is the caller's proven lower bound on elapsed milliseconds
 * since usb_start returned, not a poll count or requested sleep duration.
 * It may conservatively understate elapsed time proven by timer frequency,
 * native syscall completion and pre/post timer-state validation.
 * usb_poll never attaches while this explicit second phase is pending. */
int usb_attach(unsigned detached_ms);
void usb_poll(void);
int usb_receive(unsigned char *data, int capacity);
int usb_send(const unsigned char *data, int length);
/* Finalize a response: returns 1 only after the final packet, including a
 * required terminating ZLP, was acknowledged. Call repeatedly while polling. */
int usb_tx_idle(void);
void usb_stop(void);
int usb_is_configured(void);
/* A CONTROL_LINE_STATE request immediately invalidates the old session and
 * gates bulk traffic before status IN. DTR becomes visible only after its ACK;
 * that ACK releases RX NAK without discarding newly accepted OUT bytes. Every
 * request, including repeated DTR1/RTS changes, requires a fresh protocol PING. */
int usb_get_dtr(void);
unsigned usb_get_session(void);

#endif
