#ifndef D9288_USB_PLATFORM_H
#define D9288_USB_PLATFORM_H

/* One resident context owns this state. All calls must be serialized.
 * Claim requires the captured native firmware in its verified cold-idle state.
 * VBUS may remain high; an active or warm native USB session is still rejected.
 * Stop CDC before release. If release returns WAIT_DETACH, keep this allocation
 * alive and pass an actual elapsed detach interval to finish_release. Never
 * return/free the resident while native task3 is suspended.
 */
#define USB_PLATFORM_SOFT_DETACH_MS 250u
#define USB_PLATFORM_CLOCK_POWERUP_MS 250u
enum {
    USB_PLATFORM_IDLE = 0,
    USB_PLATFORM_CLAIMING = 1,
    USB_PLATFORM_CLAIMED = 2,
    USB_PLATFORM_WAIT_DETACH = 3,
    USB_PLATFORM_WAIT_UNPLUG = USB_PLATFORM_WAIT_DETACH, /* old numeric alias */
    USB_PLATFORM_RESTORING = 4,
    USB_PLATFORM_FAULT = 5
};
enum {
    USB_PLATFORM_OK = 0,
    USB_PLATFORM_RELEASE_WAIT_DETACH = 1,
    USB_PLATFORM_RELEASE_WAIT_UNPLUG = USB_PLATFORM_RELEASE_WAIT_DETACH,
    USB_PLATFORM_E_PROFILE = -30,
    USB_PLATFORM_E_BUSY = -31,
    USB_PLATFORM_E_TASK = -32,
    USB_PLATFORM_E_LOCK = -33,
    USB_PLATFORM_E_BUS = -34,
    USB_PLATFORM_E_SRAM = -35,
    USB_PLATFORM_E_LOST = -36,
    USB_PLATFORM_E_RESTORE = -37
};

/* A real USBCLKEN0->1 is held in snooze for a verified interval strictly
 * longer than CLOCK_POWERUP_MS before controller access. Claim may propagate
 * D88_USB_DELAY_E_* if the native timing/context proof fails. */
int usb_platform_claim(void);
int usb_platform_release(void);
/* Valid only after release returned WAIT_DETACH. detached_ms is a verified
 * lower bound on elapsed time since the first release call, not a poll count.
 * VBUS low permits immediate completion; high requires at least250ms. */
int usb_platform_finish_release(unsigned detached_ms);
int usb_platform_check(void);
int usb_platform_get_state(void);
int usb_platform_get_last_error(void);
/* First failed protected MISC write in this claim: expected<<8 | actual.
 * Cleared at claim entry and retained through rollback/restoration. */
extern volatile unsigned usb_platform_misc_failure;

#endif
