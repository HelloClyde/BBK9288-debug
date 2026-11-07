# 9288 USB platform evidence and current boundary

The USB controller implementation in `device/usb_cdc.c` follows the Epson
S1C33L05 Technical Manual, VI-2: USB function controller, CPU FIFO access,
descriptor reply and automatic address setting. It has a CDC ACM descriptor,
EP1 IN bulk, EP2 OUT bulk, EP3 IN interrupt, and EP0 of 64 bytes. PIO calls are
bounded at 64 bytes and do not use the DMA engine. The CDC transport itself
uses polling; the separate ownership layer installs the resident timer ISR.

Source manual: https://www.epson.jp/prod/semicon/pdf/id000446.pdf
English Epson manual mirror:
https://device.report/m/6203ac6c034a4fa5ccfe4a355f0ed613aca1896e64f237f404b0f081f5665e12.pdf

The standalone driver defaults to `D9288_USB_PLATFORM_VERIFIED=0`:
`usb_start()` returns `USB_CDC_E_PLATFORM_UNVERIFIED` before any register access.
The current production service build explicitly enables this macro and
`D9288_USB_TIMER_GUARDED=1`. Its strict `usb_platform_claim()` accepts only the
captured firmware and observed native cold-idle state; VBUS may remain high
after the user exits native disk mode. A warm or active MSC state is rejected. It
validates the existing BCU mapping, suspends native USB task3, masks the USB
interrupt and installs the heap-resident Timer3B guard before enabling the USB
clock. Native GUI key waits can re-enable Timer3B, so ownership requires the
guard vector to remain installed rather than relying on its EIR mask alone.
Stop soft-detaches CDC and restores the original vector, task and clock/interrupt
state after a verified interval of at least250ms, or immediately after unplug.
See `usb-platform-takeover.md` for the sequence and restoration boundaries.
F20 bit5 is USBSNZ (snooze), not a controller reset; bit4 is USBCLKEN.
Each F20 RMW obtains the one-use F2F=96 write-protection key inside its own
short CPU lock and verifies the readable bits before proceeding. The first
failed readback survives rollback as `usb_platform_misc_failure` for diagnosis.
The model does not clear USB registers or SRAM on a snooze transition.
For an actual USBCLKEN rise, the platform keeps USBSNZ set while the native
timing helper proves more than250ms elapsed, then clears USBSNZ before touching
the controller (VI-2-24, T1+250ms<T2). This clock-powerup wait is separate from
the later D+ soft-detach interval. A continuously enabled clock does not need
another powerup wait during restoration.
The driver rejects an active controller or enabled native pullup. After strict
platform ownership is established, cable power/VBUS may remain high:
`usb_start()` prepares endpoints while keeping ActiveUSB=0 and D+ pullup off.
The runtime must yield to ROS for a proven minimum250ms, recheck ownership,
then call `usb_attach(verified_minimum_ms)` to reconnect. A duration argument
below `USB_CDC_SOFT_DETACH_MS` is rejected; pending events and `usb_poll()`
cannot bypass this explicit attach phase. The argument is the caller's
verified elapsed-time lower bound, not a requested sleep duration or poll count.
The service does not forcibly detach a mounted Windows disk or reset the CPU
clock. Explicit build gates and successful model tests do not establish
physical USB operation.

Physical imagef156bf41 has enumerated and passed strict PING/STATUS, screenshot,
kernel RAM read, native GUI LEFT/RIGHT and private scratch read/write/restore.
Repeated DTR open/close,
all transfer boundaries and long-running foreground behavior remain acceptance
items. A physical stop followed by native disk
reconnection and diagnostic readback succeeded for image3f7a2a62; the latest
revision still needs that recovery check repeated.
The present QEMU board only models USB connection status and interrupt factors;
it cannot establish that real USB transactions work.

`tests/usb_cdc_model.c` uses a mock register/FIFO controller and is compiled in
all four combinations of the platform-verified and timer-guarded macros. Both
disabled-platform combinations must make zero MMIO accesses. The enabled
strict model rejects an enabled native timer IRQ; the enabled guarded model
allows that bit to be re-enabled while platform ownership protects its vector.
The enabled models check
descriptor lengths, delayed configuration commit, DTR session changes, bounded
bulk reads/writes, final zero-length packet acknowledgement, line coding,
endpoint halt/clear and bus reset. All four variants and S1C33 compilation
pass. The model does not simulate electrical USB timing or the native firmware.
It additionally checks high-VBUS preparation and complete CDC operation,
early-attach rejection with D+ still off, pending-event isolation, attach-time
clock/BCU/IRQ validation, later cable insertion, and cancellation before attach.

## Current physical device profile (2026-11-03)

The v3 report was successfully generated on the physical device and copied back.
Its SHA-256 is
`fb4e4c18c1ea71617ba2f4c8508b6e9ea8a82556b945408c9d58d327e525eac1`.
The report CRC32 is valid, profile2 matches, and all three 40-byte signatures
match (mask7). It physically observes task slots2,4,7,8 unused, native task3
waiting7, both USB phase words and desktop state zero, I/O base pointer00040000,
GUI acknowledgement byte zero, MISC27, and Timer3B/USB enables44/40. These are
device observations; the launcher still checks live state before claiming a
task slot or the controller. The later hardware USB diagnostic below records
one MSC reconnection for3f7a2a62 and CDC/data checks forf156bf41.

The earlier v2 report copied from the physical device has SHA-256
`1ef4be1409401c427d45d5b34578b5ebb72e5b65e1672070989231d072111aa9`.
Its CRC32 is valid and the PC copy matches the source on the device.
The actual tables are ROS33=0x021e81e8, GUI=0x021e7b54, FS=0x021e7ae0,
Audio=0x021e79d0, CRTL=0x0200021c, Dict=0x021e7a3c. All six tables and
the four FS function addresses match the existing verified v7 NAND capture.
The previous probe failed its hard-coded local-release table check before
creating its report. The v2 baseline successfully exposed that mismatch.

Stripping OOB from physical pages256..1919 gives a direct RAM body mapped at
0x02000000, size3407872, SHA-256
`13a2977847d105508e43e03a07260b4cc253599066b0248c75948043ada10408`.
Its entry is0x0215ebc4 and internal version strings say V1.3. The installation
package name must not be used to infer that this is the older local-release
kernel. This private body is not included in the development package.

The captured profile has vcre_tsk=0x021ced06, TCB-pointer-global=0x022a9820,
malloc=0x021d330c and heap state at0x02069000. Native initialization aligns
the heap to0x02500000..0x02700000. Native USB task3 has entry0x0219c1f2,
initial stack0x0202a394 and priority7 (stored index6); Timer3B ISR is
0x0219c9ac. Native state locations are desktop0x024ef004, phase1=0x02049798,
phase2=0x020497e0, I/O base pointer0x0204979c, and GUI flags0x02000350.
These are independently identified addresses, not an offset applied to the
old profile. Kernel task stacks in low RAM are reserved, not scratch space.

Probe v3 selects one of the two table layouts and requires three 40-byte code
signatures before reading profile-specific globals. A mismatch produces only
a baseline report. Both profiles and each damaged-signature case have native
production-C model tests. The successful physical v3 inventory above validates
the captured profile and idle baseline; it is not physical USB debug verification.

An independent full-firmware emulator check confirmed that a synthetic NAND's
FAT-root old kernel.bin was preferred by the original second-stage loader.
Renaming only that candidate in a test copy lets the true ROM path enter the
captured kernel and reproduce the physical table layout. With USB disconnected,
the model observes native USB state idle, heap bounds as above, and task3 with
the expected entry/stack. The physical v3 report independently confirms those
idle conditions and unused slots. The later native-ROS resident regression
also exercises two starts/stops in the same allocation and continued polling
after overwriting the download window, with only USB registers/SRAM simulated.
No emulator result is evidence of physical CDC enumeration or timing.

## Physical USB diagnostic (2026-11-04)

Image3f7a2a62 produced a valid CRC32 report, archived as
`D88U01-8cdccbdf2d5b.json` (private historical capture, not distributed).
It records RUNNING,22976 worker polls,4 SETUP requests and no platform error.
The last request asks for the device descriptor with wLength64; its18-byte
reply had not completed (descriptor cursor0040, remaining0012). EP0IN=10
has EnShortPkt clear. Windows still reported Code43. The service subsequently
stopped, native disk mode reconnected, and the report was read back.

The old reply code cleared the descriptor engine's automatic EnShortPkt by
writing EP0IN=08 after arming. The current driver sets direction/DATA1/NAK
before arming and then clears only ForceNAK with `bclr`; the model reproduces
the old failure. The launcher also waits when a stop is requested instead of
displaying stale READY. Corrected imagef156bf41 subsequently enumerated on
COM3 (VID:PID1209:9288, location1-5), passed strict PING/STATUS, read64 kernel
bytes matching the captured image, transferred a320x240/76800-byte screenshot
in15.974 seconds, and passed512-byte private scratch read/write/restore.
The initial screenshot ended with157 requests and0 protocol errors. A single
LEFT DOWN/UP moved the native menu highlight from D88DEBUG to D88PRB3; a single
RIGHT restored it, confirmed by screenshots without ENTER. Final STATUS was
474 requests,0 errors and no active snapshot. This is native GUI input
acceptance, not keyboard-matrix support for arbitrary games. Local
evidence is in `captures/2026-11-04-ep0-short-ready-check/`; private capture
files are excluded from distribution. This successful run does not isolate a
single physical root cause for the older failure. STOP and native disk recovery
have not yet been attempted onf156bf41. See `usb-controller-review.md`
for the register sequence and native-code evidence. Before running a newly
installed build, normally reboot to clear the previous resident; USB power
may stay on.

## Older local-release firmware notes

The local original 9288 V1.5 `kernel.bin` has SHA-256
`6a2f50e12ba31574c09d714518e9ff3029f01977a4b57fbaa7c0f77bde57d9df`.
The relocation table at 0x02000200 has GUI=0x02180058,
ROS33=0x021806ec and CRTL=0x0200021c. The GUI table spans 1684 bytes
before ROS33. Under the SDK's actual preprocessing, `_uartopen` has offset
1688 and `AttachEnable` has offset1820: these appended SDK functions are
outside this firmware GUI table and must not be called. In particular,
the presence of a header declaration is not a usable USB or UART backend.

ROS33 vcre_tsk is table index48, address0x0216715e; its ID/priority range is
1..8. It writes a TCB without testing whether the slot was already occupied.
The TCB array pointer is at0x02240d88 with a stride of72 bytes. Boot code
0x020f60c6..0x020f611c creates IDs1,5,6,3 with priorities6,5,8,7.
Other subsystems may create tasks dynamically, so these observations do not
prove slot8 is free. Inspect live slot state before using any background slot.

Further native disassembly identifies the boot-created task3 as the USB service:
entry0x02133648, priority7, created0x020f6108..0x020f611c and started at
0x020f6132..0x020f6138. Its worker accesses the USB registers even without a
USB interrupt. Therefore masking one USB interrupt alone does not establish
controller ownership. The native service is also called by the Timer3B ISR,
vector42 at0x020000a8 ->0x02133df4 ->0x02133648. Its enable is EIR3
0x00040273 bit0x40. The early strict driver gate required that timer interrupt
to be masked too; the current captured-profile service uses the resident
vector guard described above.
The neighboring Timer2B keyboard interrupt must be preserved.
This historical analysis established the need to suspend native USB task3 and
preserve its timer/interrupt and resume lifecycle. The initial foreground
prototype did not implement that takeover. The current resident ownership layer
targets the captured profile above; these older firmware addresses are reference
material, not an additional supported production profile.

CRTL malloc is0x0216b764; heap state is at0x022a9fac..0x022a9fbc.
The sole static call found to its initializer0x0216b868 is at0x020f5f92,
with heap bounds0x02500000..0x02700000. This supports a system heap, but
does not alone prove lifetime across every app loader path or device reset.
The downloaded application window begins at0x02700000 and is overwritten
by the next downloaded application. A resident service must relocate all
code, literals, globals, stack and any callbacks outside that window.
