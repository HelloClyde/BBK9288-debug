#ifndef D88_USB_LOG_H
#define D88_USB_LOG_H

#define D88_USB_LOG_MAGIC 0x55383844u
#define D88_USB_LOG_VERSION 1u
#define D88_USB_LOG_BYTES 1024u
#define D88_USB_LOG_REGISTERS 88u

typedef unsigned int d88_usb_log_word;
typedef struct d88_usb_log_register {
    d88_usb_log_word address,value;
} d88_usb_log_register;
typedef struct d88_usb_log_report {
    d88_usb_log_word magic,version,bytes,image_id;
    d88_usb_log_word flags,register_count,capture_kind,header_reserved;
    d88_usb_log_word control_words[17],control_pad[7];
    d88_usb_log_word diag_words[27],diag_pad[5];
    d88_usb_log_register registers[D88_USB_LOG_REGISTERS];
    d88_usb_log_word reserved_words[15],crc32;
} d88_usb_log_report;

enum {
    D88_USB_LOG_E_ARGUMENT=-1,
    D88_USB_LOG_E_FS=-10,
    D88_USB_LOG_E_FILES_EXIST=-11,
    D88_USB_LOG_E_EXISTING_CLOSE=-12,
    D88_USB_LOG_E_CREATE=-20,
    D88_USB_LOG_E_WRITE=-21,
    D88_USB_LOG_E_CLOSE=-22,
    D88_USB_LOG_E_READ_OPEN=-23,
    D88_USB_LOG_E_READ=-24,
    D88_USB_LOG_E_COMPARE=-25
};

/* Launcher-only, serialized call. Caller pauses its own resident worker and
 * keeps native USB task3/IRQ ownership throughout native filesystem calls.
 * This function reads no MMIO and does not change USB or CPU-lock state.
 * Copies the report, then fills magic/version/bytes/CRC in that private copy.
 * out_path is empty before selecting a free A:\D88U01..99.BIN slot; once
 * selected it remains available even on a write/readback failure. Existing
 * readable files and partial reports from previous calls are preserved. */
int d88_usb_log_save(const d88_usb_log_report *report,char out_path[24]);

#endif
