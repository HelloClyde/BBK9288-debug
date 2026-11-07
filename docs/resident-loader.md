# S1C33 resident image loader

`device/resident_loader.c` copies an audited image from the download program
into an explicitly allocated system heap block. It does not create a ROS task,
invoke resident entry, install a hook, or change USB hardware. The launcher owns
that lifecycle. The returned `memory` is the aligned relocation base;
`allocation` is the raw allocator result, retained separately.

Once a task, callback, queued event, or saved return address can reference the
image, its raw allocation must remain alive. Stopping USB is not proof that all
such references have gone. The intended service keeps its stopped image and
dormant task for reuse and reclaims them only at a device reboot.

## Build audit

`tools/build_resident.py` uses the patched BBK R6–R9 S1C33 compiler and links
with `device/resident_link.ld` and `--emit-relocs`. It rejects undefined symbols,
symbolic references outside the image, unexpected allocated sections and
unknown relocation types. Native firmware and MMIO addresses must come from
explicit numeric casts or runtime table values copied into the resident; the
resident must not reference launcher globals at `0x02700000`.

Runtime patches support `R_S1C33_32` and `ABS_H/M/L` (types2/3/4/5). The compiler's
relative call/jump triples `REL_H/M/L` (7/8/9) are checked for complete triples
and image-internal code targets and retain their linked displacement.

Every build is independently linked at two different bases,
`0x02501230` and `0x02634560`. Applying the relocation model to the original
image must reproduce both independent links byte for byte, including function
and data pointers. The generated C descriptor contains CRC32 values for both
the source bytes and sorted relocation records. The production C loader checks
all metadata, checksums and original instruction encodings before writing any
destination byte, then copies the initialized bytes, zeroes BSS and patches.

The generated header exports `D88_RESIDENT_ENTRY_OFFSET`,
`D88_RESIDENT_CONTROL_OFFSET` and `D88_RESIDENT_WORKER_OFFSET` when those symbols
exist; `--export` makes a named symbol mandatory.

## Validation scope

`tests/test_resident_loader.py` executes the production loader as a native DLL.
It tests pointer/instruction relocation, BSS and 8KiB stack storage, independent
S1C33 links, raw/aligned allocator ownership, CRC and encoding failures,
out-of-range and overlapping patches, external symbols, unknown relocation
types, malformed relative triples and preservation of output on failure.

The production service combines `resident_runtime.c`, `debug_core.c`,
`usb_cdc.c`, `usb_platform.c`, `input_jobs.c`, `usb_delay.c` and `usb_timer_guard.S` as one
closed image and checks both independent-link comparisons. Sizes and hashes
are build-specific; see `dist/service-build.json` and the resident manifest.

The captured native-kernel integration completed two start/stop cycles using
the same retained allocation. Each cycle overwrote the entire download window
at `0x02700000..0x027fffff` while the heap worker continued polling. The second
start reached `starts=2`, and both stops restored the native USB task, vector,
clock and owned interrupt bits. The current guest regression keeps its transport VBUS high through both
start/stop cycles and records native ROS counter intervals before attachment
and native resume. Native QEMU USB remains disconnected: no native mounted MSC
session is emulated. It replaces USB registers/FIFO SRAM with a guest model,
and explicitly stimulates Timer3B for the guard check; it does
not establish physical CDC enumeration, SRAM retention across hardware reset,
or physical USB/MSC recovery. The observation and source manifests remain in
`build/native-service-model/` locally.

## Captured kernel allocator evidence

For the captured kernel body with SHA256
`13a2977847d105508e43e03a07260b4cc253599066b0248c75948043ada10408`,
the allocator is `0x021d330c`, free is `0x021d353a`, initialization is
`0x021d3410`, and global state starts at `0x02069000`. The only static relative
call found to the initializer is boot code `0x0215ed62`, aligning the heap to
`0x02500000..0x02700000`. Direct GP-relative writes of the heap start/end were
found only in that initializer; other allocator-state writes belong to
malloc/free. Allocation directory entries are pointer/size-and-used-bit pairs,
with no task-owner field identified. Free searches for the supplied pointer.

Two native download paths read payload bytes into `0x02700000` and call that
address at `0x02160206` and `0x02160e82`. The inspected return paths restore
window state and clear window message chains. No global heap reset or
task-based bulk allocation cleanup was identified in those paths. The original
SDK's `Down_Ctr0.c` also leaves its `ansi_InitMalloc` call commented out.

These observations support using the system heap across ordinary download
program returns. They do not guarantee that an arbitrary third-party program
will never explicitly call the exported initializer or otherwise corrupt
memory. The native allocator itself has no internal ROS locking identified;
the launcher should serialize allocation/free while the device is idle, and
the running worker should avoid allocation.

## Captured ROS33 task context

Static inspection of `sta_tsk` at `0x021cf9b2` shows an 80-byte initial
context below the supplied stack top. The saved PC is at context+0x4c and
PSR is0x10 at+0x48. Sixteen words at+0x08..+0x44 are cleared, including r15.
The dispatcher restores ALR/AHR, r0..r15 and then PSR/PC with `reti`, so the
worker begins with r15/GP=0 and SP equal to the original stack top. A 16-byte
aligned static BSS stack therefore satisfies this initial frame. A task entry
must terminate via the OS and must not return as an ordinary C function.

The final dispatcher checks the OS inhibit bits, records pending dispatch
while inhibited, and returns. This alone does **not** establish that calling
`sta_tsk` under `dis_dsp` is safe. The native-kernel model subsequently exposed
a separate side effect: instruction `0x021cfa56` writes the new task's TCB to
`0x02068664`, which the dispatcher also reads as the current task pointer.
With the launcher still using the GUI task's stack, the next dispatch saves
that stack into the new task's `saved_sp`, replacing its correct initial
frame. Breakpoints before this write and at `0x021d0f76` confirmed the pointer
change while SP remained on the GUI stack. The evidence is in
`build/native-service-model/observations-break-021d0f76.json`; this is a model
execution of the captured native code, not a physical lifecycle test. A
production start sequence accounts for this behavior by checking the captured
code signatures, entering `loc_cpu`, saving the current TCB, creating/starting
the owned task, restoring the previous current TCB, and calling `unl_cpu`.
The final two-cycle native test used this wrapper without GDB corrections.

The same disassembly audit found no write to this current-task pointer in
`sus_tsk` (`0x021cfe60`), `rsm_tsk` (`0x021cfed2`), `loc_cpu`
(`0x021ce8ae`), or `unl_cpu` (`0x021ce8d8`). `loc_cpu` sets the dispatch
inhibit byte's low two bits and clears PSR.IE; `unl_cpu` clears those inhibit
bits, services pending dispatch, and re-enables interrupts. Unlike
`dis_dsp`, this can keep interrupts masked across a guarded native-start
wrapper's pointer restoration. See
`build/resident-loader/ros-target-api-disassembly.txt` for the captured code.

The native model also showed that priority 8 is shared with the idle task.
The idle task stays at the front of that queue, so a newly created worker at
the same priority can remain READY indefinitely. The worker must have a
priority above idle while allowing the GUI and its input handling to run.
The production worker uses priority 7 and delays between bounded poll batches.

`vcre_tsk` itself writes the selected slot without occupancy checks, so the
caller must validate the live slot before creating a task.

The OS checks dormant **bit4** when starting an existing task, rather than
requiring the complete state byte to equal0x10. A retained stopped task should
follow the OS's qualification and verify its owned entry and stack; lower
state bits can survive task termination and dispatch. These are static code
findings and do not substitute for the physical task lifecycle test.

## Conservative software USB detach delay

`device/usb_delay.c` exports `d88_usb_detach_delay()`. The caller first disables
USB attachment and keeps controller ownership; it then calls this helper
outside CPU/dispatch locks. Zero means at least `D88_USB_DETACH_MS` (250ms)
has elapsed. Every negative result means that the delay was not established
and native USB ownership must not be restored on the assumption it elapsed.

The captured `dly_tsk` at `0x021cf58e` adds its parameter directly to native
time at `0x02068674`. `sys_clk` at `0x021d11b8` increments that time by the
constant at `0x022a981c` (1), through Timer2B ISR `0x02198dbc`. The SDK's
`INT_TIME=1` comment does not establish milliseconds. Native timer setup at
`0x02198d64` uses divisor4096 and compare292. The
[S1C33L05 manual](https://www.epson.jp/prod/semicon/pdf/id000446.pdf),
PDF pages22,198,200,229, bounds OSC3/PLL input at 48MHz and specifies those
timer fields. Thus each period is at least `292*4096/48000000` seconds.
Allowing an arbitrary starting phase, 63 full periods already exceed 1.56s;
the 64-unit wait conservatively exceeds 250ms without an exact tick duration.

The helper checks the complete captured delay, clock and Timer2B ISR code,
their API/vector addresses, increment1, enabled internal Timer2B with the
known divisor/compare/control, prescaler power, disabled legacy 8-bit timer0
IRQ (whose ISR also calls `sys_clk`), and ordinary task context with interrupts
enabled. After native `dly_tsk(64)` returns0 it repeats those checks, requires
unchanged clock configuration, and verifies at least64 native time units
advanced. A nonzero native return, short/backwards time, changed signature,
timer or context fails. No USB or timer register is written by this helper.

`tests/test_usb_delay.py` executes production C with a native-call model. Its
nine tests cover every signature byte, bad profile/timer/context, early native
return, changes while sleeping, insufficient time, and 32-bit time wrap. They
do not measure physical USB disconnect timing. The S1C33 helper also compiles
with `-Wall -Wextra -Werror`. Local evidence is in
`build/resident-loader/ros-tick-research-disassembly.txt`,
`native-tick-timer-init.txt` and `timer-init-xrefs.json`.
