#ifndef D88_USB_DELAY_H
#define D88_USB_DELAY_H

#define D88_USB_DETACH_MS 250u
#define D88_USB_DETACH_TICKS 64u

enum {
    D88_USB_DELAY_E_GUARD=-201,
    D88_USB_DELAY_E_CONTEXT=-202,
    D88_USB_DELAY_E_INTERRUPTED=-203,
    D88_USB_DELAY_E_CHANGED=-204
};

/* Caller has already disabled USB attachment and retains controller ownership.
 * Call outside CPU/dispatch locks. Zero guarantees the conservative lower
 * bound above for the captured timer configuration; every failure means the
 * caller must retain ownership instead of claiming that detach has elapsed. */
int d88_usb_detach_delay(void);

#endif
