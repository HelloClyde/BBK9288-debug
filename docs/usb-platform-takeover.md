# Captured 9288 USB ownership layer

This describes the experimental implementation in `device/usb_platform.c`.
It has native production-C model regression and S1C33 compilation. Image
3f7a2a62 subsequently stopped on hardware, returned to native disk mode and
allowed its valid D88U01 report to be copied back. Corrected imagef156bf41 has
now physically enumerated as1209:9288 and passed strict PING/STATUS,320x240
screenshot, native GUI LEFT/RIGHT with screenshot-confirmed selection
restoration, kernel RAM read and private scratch read/write/restore. The earlier
Code43 belongs to the old build. No STOP has been sent onf156bf41, so its
physical recovery remains untested. No physical device was accessed by this
implementation subtask; the hardware results are recorded separately by the
acceptance run.

## Bound native state

The implementation accepts only the captured kernel with entry `0215EBC4`:
the six general-table entries must match profile2, and the vcre_tsk, malloc,
and native USB worker must each match 40 bytes of code. It also checks the
Timer3B vector and the four ROS function addresses it will call.

The physical v3 report `DBG88P02.BIN`, SHA256
`fb4e4c18c1ea71617ba2f4c8508b6e9ea8a82556b945408c9d58d327e525eac1`,
established the actual task3 entry/stack/priority, idle phases, BCU state,
interrupt enables and cold USB snooze/clock state. At claim time these are checked
again. The task must be in the observed waiting state7, not dormant, running,
or already suspended. The expected task3 fields are entry `0219C1F2`, stack
`0202A394`, and stored priority index6.

ROS vcre_tsk uses TCB-base pointer `022A9820`. The sus_tsk and rsm_tsk code
use aliases `022A9864` and `022A9868`; claim requires all three to be equal.
An independent original-ROM/full-firmware model boot confirmed all three
point to `02068B34`, with R15=0. That read-only observation is recorded at
`build/usb-platform/native-alias-check.json`. It is not a test of calling the
two APIs or of physical USB transport.

## Claim sequence

VBUS may remain high to power the device. The user must first exit native disk
mode, and the actual native state must meet every cold-idle condition below.
A return to the menu alone does not prove those conditions. The native USB
state, both phase
words and GUI completion byte `02000351` must be zero. The USB I/O-base
pointer must still be `00040000`. The bus must already have the observed
internal little-endian USB mapping, 8-bit bus/wait configuration and OSC3
enabled. The implementation changes no BCU register, CPU frequency or clock
source. It accepts only MISC USB state `27`: USB clock off, snooze enabled,
wait7. The other MISC bits are preserved.

After locking the CPU it checks the idle state again, suspends only task3,
verifies that its suspend bit was actually set, and masks only bit40 in both
Timer3B enable `40273` and USB enable `402A6`. It saves vector `020000A8` and
replaces it with `usb_timer_guard.S`, linked inside the retained resident.
The neighboring keyboard timer enable is preserved. A failed suspend never
causes an unowned resume. If native state changes during this stage before
controller changes, the native paths are restored immediately without modifying
the USB controller. VBUS alone is not an ownership indicator and does not
reject an otherwise valid cold state. Active MSC and warm negotiation states
remain rejected.

The native GUI wait-for-key routine at `0219B914` unconditionally re-enables
Timer3B at `0219B924`. A register-only mask therefore does not provide lasting
ownership. The replacement ISR saves R0/R1, writes exactly40 to FIR `40283`,
restores R0/R1 and executes RETI. It calls no OS function, changes no ALR/AHR,
and enables no nested interrupt. The GUI may re-enable that EIR bit while the
debugger runs; health checks require the vector to remain owned. USB's own
CPU interrupt remains masked. The CDC driver permits the timer EIR bit only
under its explicit `D9288_USB_TIMER_GUARDED` build gate.

Once both native paths are paused, it enables the USB clock while keeping
snooze enabled. Each actual USBCLKEN0-to1 transition requires a verified wait
strictly longer than250ms in this state before USBSNZ is cleared, following
VI-2-24, figure VI.2.12 and table VI.2.7: T1+250ms<T2. The existing native
`d88_usb_detach_delay()` supplies that lower bound outside every CPU/dispatch
lock; no USB register or SRAM is accessed while waiting. It then waits for
bus synchronization, leaves snooze, and verifies
ActiveUSB=0 and D+ pullup disabled. It checks revision12. It then saves the entire
1024-byte shared FIFO/descriptor SRAM through DescDoor, with the documented
initial dummy read, and reads it a second time to verify the snapshot. CDC
may subsequently replace descriptor data and FIFO regions. Merely saving
USB_Control and XcvrControl would not preserve the original MSC descriptors.
USB snooze is not a reset: it synchronizes/gates controller access and does not
clear SRAM or controller registers. The snapshot is taken after clock/snooze
wakeup and before CDC configuration. No register-file reset or SRAM-clearing
behavior is assumed. Shared SRAM is explicitly restored; a byte-for-byte
snapshot/restore of every controller configuration register is not established.

Every write to MISC `300F20` requires its own write-protection key `96` at
`300F2F`. The key authorizes one write to F20 or F30..F39; any such write consumes
it. Each F20 RMW therefore has a separate short CPU lock around key, target
write and readback. The readable mask37, including preserved USB wait bits,
must match. These helpers run outside the longer task/vector critical sections,
and no lock spans a bus delay or SRAM copy. This follows the native key/write
sequence at `0219C46A..0219C474` and the manual's WRPROT definition, III-17-10.
The first mismatch is retained in `usb_platform_misc_failure` as
`expected << 8 | actual`, even if rollback succeeds. A fresh eligible claim
clears it. Ignored wake writes are detected before CDC startup; if the controller
was not touched, cleanup restores only changed clock/SNZ state and native paths.
If the verified powerup wait fails, no controller access is attempted and clock
rollback preserves the original failure. A clock loss between wake stages cannot
raise the clock and clear snooze in one write. Normal restoration while the clock
has remained enabled needs no additional powerup wait; an actual new rise during
recovery requires the wait again. This interval is separate from the later D+
soft-detach wait and cannot be replaced by it.

## Stop and restoration

The caller first stops CDC. `usb_platform_release()` independently makes
ActiveUSB=0 and disables D+ pullup. While VBUS is high it returns1 and holds
the original task/interrupt paths paused in WAIT_DETACH. The caller supplies
`usb_platform_finish_release(detached_ms)` with a verified lower bound on
the elapsed detach interval. At least250ms is required while VBUS stays high;
repeated calls and caller-provided poll counts are not a time source. Repeated
release calls do not restart or rewrite the detach operation. If VBUS becomes
low, completion may proceed immediately. The resident must stay allocated
through the whole sequence. WAIT_UNPLUG remains only an old numeric API alias.

After the detach interval or physical unplug, completion wakes controller access,
explicitly disables ActiveUSB and pullup, restores all1024 shared SRAM bytes and
compares a readback. It verifies ActiveUSB/pullup are off again before entering
snooze and removing the clock. The original native disconnect routine at
`0219C4EC..0219C558` writes a fresh key96 before F20=F7, delays, and writes a
fresh key96 before F20=E7. Reserved bits read zero, so these correspond to37
and27. The implementation uses masked RMWs and verifies every transition,
preserving the other readable MISC fields. Failure leaves native paths guarded
and does not blindly replay restoration.

Finally, under CPU lock it clears only the paused USB Timer3B/USB pending
interrupt bits, masks Timer3B again, verifies and restores its original vector,
resumes exactly the task3 suspension it owns, and restores
only its two saved interrupt-enable bits. Changes to other peripherals' bits
are preserved. This restores the observed native cold register/task state.
One physical native mass-storage reconnection and report-readback cycle has
succeeded for image3f7a2a62. It does not prove every endpoint register was
restored or cover all recovery cases; the latest revision must repeat the
check. Restoring SRAM and returning to snooze/clock-gated mode alone is not
evidence of disk recovery.
The native worker at `0219C26C` checks VBUS and, when phase2 is zero,
calls `0219C458`. That helper enables the clock and negotiates only when the
GUI handshake flag `0200034D` is zero; otherwise it returns. The cold-exit helper
`0219C4EC` sets this flag, and a later GUI branch at `02163202` clears it.
GUI confirmation then permits `0219D160` and phase1. Re-entry therefore depends
on this handshake as well as VBUS and phase state. These are code-path
observations, separate from the physical reconnection recorded above.

The v3 report established the original cold-idle baseline without reading
the controller's VBUS status. The
user has also confirmed that the physical disk UI can be exited while USB power
stays connected. The later D88U01 capture records an active owned debug session,
not the raw native state immediately after that exit. A warm phase2=1 state is not accepted
as a substitute for the strict baseline. If menu return leaves that state warm,
the current implementation rejects it rather than taking an active controller.

If descriptor SRAM verification, task identity, resume or ownership checks fail,
the state is FAULT. The native paths remain protected and the error is retained.
Repeated release calls do not replay failed restoration writes. The resident
must retain its memory and report the failure; a device reboot is then needed.
If resuming task3 fails after restoring the vector, the resident guard is
reinstalled so a later GUI timer-enable write cannot enter the native USB
worker. A foreign vector is never overwritten during restoration.

## Regression scope

`tests/test_usb_platform.py` compiles the actual production source with an
OS/register/SRAM model. Its31 tests cover full claim/restore, all shared SRAM,
both IRQ paths, preservation of unrelated bits and keyboard flag, active
or busy rejection, wrong profile/task/bus rejection, suspend failure, cable
arrival and timed WAIT_DETACH, SRAM readback failure, resume failure, and health checks.
They also cover GUI timer re-enable and refusing to overwrite a foreign vector.
Powered cold-idle claim is accepted, powered active/warm states are rejected,
249ms cannot finish a powered release, and250ms can restore native state while
VBUS stays high. An already idle cleanup returns immediately without a delay.
The model enforces one-use WRPROT, rejects writes without a key, and simulates
a native Misc write consuming an unlocked caller's key. It verifies ignored
first/second wake writes, partial-clock rollback, persistent mismatch evidence,
non-nested short locks and lock/unlock failures. Snooze retains all modeled USB
registers/SRAM. Failed ActiveUSB/pullup readback prevents returning native paths.
Invalid clock/revision is rejected by quiesce before any controller write;
cleanup leaves the resident guard and task suspension in place rather than
writing USB registers through an invalid bus/clock state.
Powerup-delay stubs assert snooze remains enabled, CPU locks are released and
no USB MMIO precedes the initial delay. They cover early delay failure, preserved
readback evidence, clock rollback, a clock loss between wake stages, no repeated
powerup wait on a continuously enabled clock, and a required wait after a new rise.
The S1C33 build passes `-Wall -Wextra -Werror`.

`tools/build_service_model.py` builds the actual runtime, core, CDC transport,
ownership layer, native input module and assembly ISR with a guest-only USB
register/SRAM adapter. All other RAM/MMIO and the four platform ROS syscalls
use the captured firmware directly. `tools/test_service_native_model.py` boots
an independent full NAND copy, injects the real download launcher once through
the desktop callback, and exercises the actual native malloc, relocator,
vcre_tsk, sta_tsk, suspend/resume, CPU lock/unlock, delay and ext_tsk paths.

The recorded two-run regression reaches RUNNING, proves continued polling after
overwriting the entire `02700000..027FFFFF` download window, and reaches STOPPED
with task3 waiting7, the original Timer3B vector, IRQ44/40 and MISC27 restored.
It restarts the exact same build in the same heap allocation, overwrites the
download window again and stops a second time. The latest regression also
asserts that both stop dialogs show STOPPED (state5), not stale READY while
the stop request is pending. Native ext_tsk leaves raw TCB8
state13: dormant bit10 is set and its ready-queue nodes are self-linked.
The ISR itself is reached using the real QEMU timer peripheral; the fixture
temporarily starts Timer3 via PRUN and restores its prior control value after
the guard breakpoint. This extra IRQ stimulus is recorded explicitly.
Logs, source hashes and all before/after snapshots are saved in
`build/native-service-model/observations.json` and `model-build.json`.

The current regression keeps guest transport VBUS high across both cycles,
with 65 native ROS counter units recorded before each software attach and
before each native task resume. The native QEMU USB window remains disconnected;
the fixture does not exercise a real mounted native MSC session or its exit.

The model is not evidence of electrical USB timing, silicon FIFO behavior,
or a successful return to the Windows mass-storage driver. The physical
3f7a2a62 stop/readback observation is separate evidence; future builds still
require physical acceptance bound to their executable.
The corrected T1-toT2 sequencing follows the manual; it has not established the
physical cause of the observed Windows descriptor-request failure.

The current launcher waits for RUNNING with a pending stop request to advance
to STOPPED, or shows USB WAIT if restoration remains in progress. The former
READY display race could show stale startup text after requesting stop and
saving a report. Updating the executable does not replace that old heap image:
normally reboot the device before running a new build. The USB cable can stay
connected for power throughout the update/retest workflow.
