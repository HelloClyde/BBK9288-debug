#ifndef D88_MEMORY_SPEED_H
#define D88_MEMORY_SPEED_H

#define D88_MEMORY_SPEED_VERSION 1u
#define D88_MEMORY_SPEED_READ 1u
#define D88_MEMORY_SPEED_WRITE 2u
#define D88_MEMORY_SPEED_COPY 3u
#define D88_MEMORY_SPEED_WRITE_WORD 0xa55a3cc3u
#define D88_MEMORY_SPEED_WINDOWS 4u
#define D88_MEMORY_SPEED_WINDOW_TICKS 32u
#define D88_MEMORY_SPEED_RTC_MODULUS (3600u*256u)
#define D88_MEMORY_SPEED_FLAG_NOMINAL_RTC 1u
#define D88_MEMORY_SPEED_FLAG_INTERRUPTS 2u
#define D88_MEMORY_SPEED_FLAG_YIELD_EXCLUDED 4u
#define D88_MEMORY_SPEED_FLAG_TIMER2_CHANGED 8u

typedef struct d88_memory_speed_result {
    unsigned version,operation,source,destination,bytes_per_pass,windows;
    unsigned passes,bytes_processed,rtc_ticks,rtc_nominal_hz,flags,checksum;
    unsigned error,ros_ticks,t2_first,t2_last,t2_compare_b,t2_ctrl,t2_clock;
    unsigned psc_select,psc_power,clock_option,rtc_first,rtc_last;
} d88_memory_speed_result;

enum {
    D88_MEMORY_SPEED_E_ARGUMENT=-501,
    D88_MEMORY_SPEED_E_OWNER=-502,
    D88_MEMORY_SPEED_E_CONTEXT=-503,
    D88_MEMORY_SPEED_E_CLOCK=-504,
    D88_MEMORY_SPEED_E_COHERENCE=-505,
    D88_MEMORY_SPEED_E_YIELD=-506,
    D88_MEMORY_SPEED_E_STALLED=-507,
    D88_MEMORY_SPEED_E_CHANGED=-508
};

/* Fixed native kernels, never a host-supplied program. READ permits 16/64KiB
 * SDRAM, or 16KiB within IRAM 0..3FFF / IVRAM 3C0000..3C9FFF. WRITE/COPY
 * require 64KiB SDRAM and an owns() callback checking every complete window.
 * COPY windows must not overlap. READ destination and WRITE source are 0.
 * The same code serves every bank. The caller owns buffer lifetime and
 * prepares/checks data outside these timed windows. No register is written.
 * rtc_ticks excludes explicit ROS yields, but includes interrupts/preemption
 * during each continuous window. 256Hz is nominal OSC1/128, not a calibrated
 * frequency claim. Preserve raw results until an independent clock check.
 * ros_ticks and Timer2 samples are calibration evidence, not milliseconds. */
int d88_memory_speed_run(unsigned operation,unsigned source,unsigned destination,
    unsigned bytes,int (*owns)(unsigned address,unsigned bytes),d88_memory_speed_result *out);
/* Same RTC units as each result, modulo one hour. This never starts a timer.
 * Whole-run before/after samples can independently cross-check the nominal
 * frequency; host RPC durations are not memory-transfer elapsed times. */
int d88_memory_speed_clock(unsigned *rtc_ticks,unsigned *ros_ticks);

#endif
