# USB controller and GUI queue review, 2026-11-04

This is an offline review of the captured 9288 kernel and the production C
driver. No physical device or mounted device disk was accessed in this subtask.
The controller model is a regression tool, not proof of physical enumeration.

## Physical diagnostic and latest correction

The hardware report from image `3f7a2a62` is archived as
`D88U01-8cdccbdf2d5b.json` (private historical capture, not distributed).
Its1024-byte BIN has valid CRC32 and SHA256
`8cdccbdf2d5bf6ba562b206a427ffa855b6f73044b6930e046e008528d0fd6b2`.
At capture the worker was RUNNING with no platform error,22976 polls and4
SETUP requests. The final request was `80 06 00 01 00 00 40 00`; the descriptor
address/remaining length stayed at0040/0012, EP0Control was81 and EP0IN was10.
The18-byte device descriptor reply had not completed and Windows reported
DEVICE_DESCRIPTOR_FAILURE/Code43. After stopping, the physical native disk
reconnected and the report was copied back. This is a hardware observation of
that stop/recovery cycle, not successful CDC enumeration.

The old descriptor reply armed ReplyDescriptor and then wrote EP0IN=08.
VI-2-14 specifies that the engine automatically enables EnShortPkt; the later
full-byte write cleared that bit. The corrected sequence sets EP0Control=80
and EP0IN=0A first, loads the descriptor address/data/length, arms with81, then
uses a single peripheral `bclr` to clear only ForceNAK bit1. This preserves
the engine's automatic short-packet enable without a C read/write gap.
The model now treats a short IN with EnShortPkt clear as NAK without consuming
the descriptor. It explicitly reproduces the old arm-then-W08 failure and
checks successful8/18-byte replies and the final3 bytes of a67-byte reply.
This identifies a concrete driver bug; it does not establish short packets as
the sole physical cause of the old Code43. Corrected imagef156bf41 subsequently
enumerated on the physical device as COM3,1209:9288, location1-5, and passed
strict PING identity and STATUS checks. A64-byte kernel read matched the
capture, a320x240/76800-byte grayscale screenshot completed in15.974 seconds,
and512-byte private scratch pattern read/write passed with the original bytes
restored. After the initial screenshot STATUS recorded157 requests,0 errors.
Single LEFT DOWN/UP changed the native menu highlight from D88DEBUG to D88PRB3;
single RIGHT DOWN/UP restored D88DEBUG. Three original-size screenshots
confirmed the change and restoration, with no ENTER sent. Final STATUS was
474 requests,0 errors and no active screenshot token/bytes. This validates
those native GUI menu inputs, not arbitrary games' keyboard-matrix polling.
Local evidence is in `captures/2026-11-04-ep0-short-ready-check/acceptance.json`,
`functional-check.json` and `screen-before.png`; these private captures are not
distributed. STOP has not been sent for this build, so its physical recovery
still needs a separate test. A normal reboot is required before running a
different installed build; the USB cable may remain connected for power.

Captured native code independently supports the access sequence: descriptor
preload at0219CD6A..CD7E starts88/89 at0040 and writes bytes directly through8F;
the IN SETUP path at0219DE1E..DE38 sets EP0Control bit7. The descriptor reply
at0219DEF4..DF1C sets88..8B,0219DFD4 writes the old EP0Control OR1, and
0219DFEC clears only EP0IN bit1. It does not use CPU_ACCESS82/Join for this
descriptor-engine branch or explicitly set EnShortPkt. Offline disassembly
is in `build/resident-loader/native-usb-desc-and-direction.txt` and
`build/usb-platform/native-descriptor-reply-disas.txt`.

The launcher also fixes a separate display race: RUNNING with
`stop_requested=1` is now a stopping state for the report wait. It displays
STOPPED after completion or USB WAIT while restoration remains pending,
rather than a stale READY. The earlier READY after requesting stop was not
evidence that the heap worker had been lost or restarted.

## Controller changes

The primary hardware reference is the Epson S1C33L05 Technical Manual, VI-2:
https://www.epson.jp/prod/semicon/pdf/id000446.pdf

The review corrected these hardware assumptions in `device/usb_cdc.c`:

- EP0MaxSize is at offset3F, not reserved offset3C (VI-2-62).
- EPa/b/c/d Control registers occupy40/41/42/43. Clear-FIFO masks use the
  corresponding contiguous endpoint index (VI-2-63..66).
- XcvrControl81 selects non-driving operation. After VBUS rises, normal
  operation requires80; unplug returns to81 (VI-2-52). Both the event and the
  sampled VBUS level are checked so clearing an event during initialization
  cannot leave the transceiver non-driving.
- Automatic negotiation requires reset/suspend detection enabled first.
  SIE_IntEnb=F0 and USB_Control=41 enable the internal state machine while
  MainIntEnb stays0 and the separate native CPU USB IRQ stays masked
  (VI-2-18..19,47,51). InSuspend remains set until a NonJ resume event.
- A new SETUP explicitly clears AutoSetAddress. The manual says a cancelled
  automatic SET_ADDRESS operation does not clear its enable flag (VI-2-58).
  A valid address still takes effect only after the status IN acknowledgement.
- A request with wLength=0 has no data stage and uses status IN. The supplied
  descriptor payloads are all nonempty; this case therefore bypasses the
  descriptor engine. USB2.0 section8.5.3 states the no-data status direction.
- SET_INTERFACE commits at status acknowledgement and resets only the chosen
  interface's FIFO and DATA0 state. Interface1 also invalidates the bulk
  session. DTR is retained. CLEAR_FEATURE(ENDPOINT_HALT) resets that endpoint's
  DATA0 state, while DTR open/close drops application data without resetting
  a USB data toggle.
- Unsupported line-coding values no longer overwrite the previous valid
  coding before returning STALL.

Descriptor replies use the controller engine (VI-2-59,84..86). The production
configuration descriptor is67 bytes; completion is expected after64+3, not
after the first packet. CPU FIFO access selects a pipe with CPU_ACCESS off,
then enables PIO; DMA_Join is0. Remaining counts are read high then low as
specified by VI-2-79..83. Bulk responses queue at most64 bytes per call;
`usb_tx_idle()` must be used only after the complete response has been queued.
It emits a terminating ZLP only after a full final packet is acknowledged.

`tests/usb_cdc_model.c` independently rejects reserved register offsets,
models DATA toggle strobes and deferred address application, checks high/low
count latch order and rejects FIFO join changes while the relevant CPU
access gate is enabled. It exercises descriptor64+3, cancelled SET_ADDRESS,
zero-wLength status IN, DTR, short packets, intermediate full packets without
ZLP, final full packet with ZLP, alternate settings, halt clear, suspend/resume,
and line-coding rejection. The default disabled build makes zero MMIO calls.
All model builds pass. S1C33 compilation with all gate combinations passes
`-Wall -Wextra -Werror`. The physical imagef156bf41 enumeration and data checks
above establish a successful host connection independently of this model;
they do not measure all electrical/controller timing or cover every transfer
case. One physical native mass-storage reconnection/report-readback cycle is
recorded for3f7a2a62; f156bf41 has not yet run STOP/recovery on hardware.

Full native GUI testing subsequently found that GetKey path0219B914 writes
Timer3B enable40273 bit40 at0219B924. A one-time mask cannot establish lasting
timer ownership. `D9288_USB_TIMER_GUARDED` therefore defaults0, retaining the
strict timer-mask startup check. An explicit value1 delegates only that timer
check to a platform-owned heap IRQ-vector guard that must already be installed
and verified by platform claim/check. It does not relax the native USB IRQ113
mask, USB clock or BCU checks. The service build owns the decision to enable
this mode; this macro alone does not install or validate the guard.

The production USB model is compiled in all four platform-gate/timer-guard
combinations. With platform-gate0, either timer mode makes zero MMIO accesses.
With platform-gate1, strict timer mode rejects a re-enabled Timer3B bit without
any writes; guarded mode runs the entire CDC regression with that bit still
set. Both modes reject a live USB IRQ bit or invalid clock/BCU state. All four
model and S1C33 `-Wall -Wextra -Werror` builds pass.

## Windows inbox serial-driver binding

The single CDC ACM function now reports device-level class/subclass/protocol
02/02/01. Microsoft's [USB serial driver documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-driver-installation-based-on-compatible-ids)
requires device class02 and subclass02 for automatic inbox Usbser.sys loading.
The local Windows usbser.inf also directly matches02/02/01. The previous
02/00/00 did not match that rule or the [automatic composite-parent rule](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/enumeration-of-the-composite-parent-device),
which requires class00 or EF/02/01 in addition to multiple interfaces and a
single configuration. The existing control/data interfaces and union functional
descriptor stay intact; a single serial function does not need an IAD for this
direct binding path. The controller model checks the actual first8 bytes
returned by GET_DESCRIPTOR, including02/02/01 and EP0Max64.

This removes a driver-binding problem after successful enumeration. It does
not establish the cause of the physical DEVICE_DESCRIPTOR_FAILURE/Code43.

## USB-powered software detach and attach

The controller no longer treats VBUS alone as ownership or a busy condition.
Strict platform claim must already have stopped the native USB task and IRQ
paths and left the controller inactive with its pullup disabled. An active
controller or existing native pullup is still rejected before CDC writes.

`usb_start()` now only prepares: it writes ActiveUSB=0 and XcvrControl=01,
keeps D+ pullup off, clears endpoint state and enters a detached prepared
state. `usb_poll()` does no MMIO in that state, so pending VBUS/reset events
cannot inadvertently reconnect. The runtime yields to ROS, proves a minimum
`USB_CDC_SOFT_DETACH_MS` (250ms) since successful preparation, rechecks strict
platform ownership, and calls `usb_attach(verified_minimum_ms)`. The explicit
argument is a verified elapsed-time lower bound, which may conservatively
understate actual duration. The native timer frequency, successful ROS delay
completion and pre/post timer-state validation establish that bound. The
driver has no ROS clock of its own. A value below250 returns
`USB_CDC_E_DETACH_WAIT` without connecting. This interval is a conservative
host observation window chosen by the implementation, not a claim that the
USB electrical disconnect minimum is250ms.

Attach repeats clock/BCU/IRQ, revision and inactive-controller checks, clears
old events, enables automatic negotiation, and raises D+ in normal mode80 if
VBUS is already high. With VBUS low it uses81 and the normal polling path
switches to80 upon later insertion. Stop during preparation cancels attach.
The same sequence supports cable-powered native MSC exit without unplugging;
the platform must separately prove that native MSC has exited before claim,
and it owns the reverse transition and restoration on stop.

The four-macro model now runs complete enumeration/CDC regression with VBUS
high throughout preparation/attach. It checks0ms/249ms rejection, D+ remaining
off across pending events, rejection when clock/BCU/IRQ/controller state
changes before attach, an ordinary low-VBUS start followed by insertion,
and stop while waiting. Disabled platform modes also call the new attach API
and still make zero MMIO accesses. All four model and S1C33 strict builds pass.
These checks establish the driver's state/minimum-duration contract; they do
not establish runtime ROS tick units or physical host re-enumeration timing.
The runtime delay helper separately validates the captured timer and delay
routine; it must not assume that requesting250 ROS ticks means250ms.

VBUS-low attach and unplug now write USB_Control=01, clearing EnAutoNego as
required by VI-2-19. VBUS rising enables SIE reset/suspend detection before
USB_Control=41 and XcvrControl=80. A stale reset/resume event while VBUS is low
cannot re-enable automatic negotiation.

## Exported CDC diagnostics

`usb_diagnostic` is a volatile exported `usb_cdc_diagnostic` record. Its fixed
version1 layout is27 unsigned32-bit words (108 bytes), in this order:

| Word offsets | Fields |
| --- | --- |
| 0..3 | version, phase, result, failure_mask |
| 4..9 | bcu_hi, bcu_lo, misc_f20, clock_f35, usb_irq, timer_irq |
| 10..16 | usb_valid, revision, control, status, xcvr, main_status, sie_status |
| 17..24 | sie_enable, test, ep0_status, ep0_control, ep0_in, ep0_out, ep0_max, address |
| 25..26 | setup_count, poll_count |

START is phase1, ATTACH phase2, and a runtime safety failure phase3. `result`
stores the signed API result's32-bit representation. Failure bits are named
in `usb_cdc.h`; for example missing F20 clock is04, snooze08, stopped F35
module clock10, native USB IRQ20 and unguarded timer IRQ40 (all hexadecimal).
Every check samples all six platform bytes before any USB access. A failed
platform gate records its complete mask and does no USB MMIO. Only a valid
gate permits reading RevisionNum; a mismatch records that byte and stops
before reading other USB registers. `usb_valid=1` means a complete snapshot
was read under valid gates and revision12. If it is0, other USB fields must
not be presented as a current valid snapshot.

Normal polls refresh the USB register fields and increment counters without
replacing the last start/attach phase/result. The snapshot precedes that
poll's event handling. A runtime gate/revision failure invalidates the
snapshot, records phase3, and drops the software transport state so later
send/receive/idle calls cannot touch USB. Stop preserves the diagnostic
record. After a runtime safety failure or any revision failure it performs
only software cleanup; other stop paths independently recheck platform gates
before USB writes. Platform restoration remains a separate guarded operation.
The launcher can copy the record after FAILED/STOPPED; reads during active
polling are best-effort snapshots, not an atomic multiword transaction.

The production model asserts on any USB access with invalid platform gates.
It checks every failure bit, combined F20 clock/snooze failure, unknown
revision, prepared/attached snapshots, setup/poll counters, and exact record
preservation through stop. Runtime gate loss permits no further USB accesses;
runtime revision loss permits only the single revision read and then none,
including through stop. Both disabled-platform macro modes still make zero
MMIO accesses. These assertions and all four target builds pass.

## Native GUI key target

The following addresses belong only to the captured kernel body SHA256
`13a2977847d105508e43e03a07260b4cc253599066b0248c75948043ada10408`,
entry0215EBC4, GUI table021E7B54. They are not offsets applied to another
firmware profile. Supporting offline disassembly is in `build/io-review/`.

GUI table offsets are PostMessage+60 ->02182C3A, GetFocus+312 ->0217C6D8,
and GetActiveWindow+324 ->0217C75E. GetActiveWindow synchronously sends F4
to HWND_DESKTOP. The desktop procedure021813C2 first calls an optional hook
at global0203857C, so calling GetFocus/GetActiveWindow from a worker or while
CPU-locked can execute an unknown application callback.

The desktop procedure routes messages F0..10F to02180EB8. Its jump table at
021FAA6C has entryF4 at+16, value02180FBE. That branch directly reads the
active-main-window handle at02038474 and returns it, with no callback. Its
first10 bytes are `1c d0 74 c4 f4 30 00 c0 cd 1e`. GetFocus subsequently
reads activeMain+0xC4 and returns that child if nonzero, otherwise0. Its
26-byte code signature is
`00c000c0411c04680718c4c0453005680418031f542e046c4006`.

A worker can therefore use a profile-signature-protected, short CPU lock to
read activeMain/focus and queue one message without calling the query APIs.
PostMessage resolves a main-window queue and copies four32-bit message fields
into a28-byte ring slot. It has no ROS wait, callback, or internal lock. Its
only calls are the structural queue resolver and a fixed32-step integer
remainder helper021D5120. It returns0 for success, -1 for a missing queue,
and -2 for a full ring. The integer helper preserves the PSR it sees while
entered, including the caller's interrupt-disabled state.

Prevalidation is necessary: resolver02182342 calls0218231A and then reads
main+0xDC without checking a null main result. A stale invalid HWND cannot
safely be handed to PostMessage as an ordinary error case. Under the same
short lock, validate the target's RAM range and halfword type1. A main has
halfword+2 equal11; a child resolves its owning main through+0xB8. Validate
the owning main, its queue pointer+0xDC, queue ring pointer+0x10, capacity
+0x14 and read/write indices+0x18/+0x1C before calling. Bound the capacity and
entire ring range. The GUI worker cannot be preempted by an IRQ producer while
the CPU lock is held. Do not retain a DOWN handle across arbitrary window
destruction without revalidating it before UP, and do not redirect UP to a
newly focused window. Reused HWND allocation addresses are still a lifetime
limit that needs runtime handling/physical acceptance.

An independent full-firmware emulator observation found hook0203857C=0 at
desktop idle and after a downloaded application returned. That observation
does not guarantee future applications leave the hook null. The direct
active-main read avoids depending on it.

## Production input module regression

`device/input_jobs.c` now fingerprints all144 bytes of native PostMessage,
the64 bytes containing both owner/queue resolution helpers, and the38-byte
integer remainder helper. All six signature arrays were compared directly
against the captured kernel body. A nonzero invalid focus is rejected rather
than silently redirected. Each pending key records its window's owner,
message queue and kind, and must still match these when retrying or sending
UP. Unlock failures on the invalid-target paths are reported as lock errors.

`tests/test_input_jobs.py` compiles the actual source through the test shim
in `tests/input_jobs_model.c`. Its15 tests cover desktop/main/focus selection,
same-window DOWN/UP despite focus changes, a full ring before and after DOWN,
window destruction, changed owner/queue/kind, invalid pointer/type/ring/count
or index rejection, code/profile corruption, cancellation, scan bounds,
lock/unlock failures and native error propagation. Fingerprints are checked
again after taking the CPU lock, and a simulated GUI code-hook race between
the initial check and lock must produce zero POST calls. The model asserts every
POST and every GUI structure read occurs inside the CPU lock. It records
attempts separately from accepted messages and updates the actual modeled
ring, so queue-full retries cannot hide duplicate accepted DOWN messages.
All tests and S1C33 `-Wall -Wextra -Werror` compilation pass.

A separate original-ROM/captured-kernel emulator boot observed idle activeMain
0253A3C8, header halfwords0001/0011, focus0, and main+DC=020383D0. That queue
had ring02539F8C, capacity16, read/write indices8/8. The observation is saved in
`build/io-review/input-native-gui.json`, and these exact structure fields are
also exercised by the production-reader model. No native GUI function was
called in this observation. Actual target lifetime and delivery on the
physical device remain physical acceptance steps; in particular identical
addresses/owner/queue/kind reused for a new window cannot be distinguished
without an additional native allocation-generation mechanism.
