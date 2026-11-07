#ifndef D88_POWER_IDLE_H
#define D88_POWER_IDLE_H

#define D88_POWER_IDLE_DIAG_VERSION 1u

typedef struct d88_power_idle_diagnostic {
    unsigned version,active,refreshes,last_error;
} d88_power_idle_diagnostic;

extern volatile d88_power_idle_diagnostic power_idle_diagnostic;

enum {
    D88_POWER_IDLE_E_TABLE=-401,
    D88_POWER_IDLE_E_ENTRY=-402,
    D88_POWER_IDLE_E_CODE=-403,
    D88_POWER_IDLE_E_ROS=-404,
    D88_POWER_IDLE_E_CONTEXT=-405,
    D88_POWER_IDLE_E_LOCK=-406,
    D88_POWER_IDLE_E_PENDING=-407,
    D88_POWER_IDLE_E_UNLOCK=-408
};

/* Refresh the captured firmware's idle counter, preserving its user setting,
 * MSC state and GUI timers. Begin enables periodic refresh only on success.
 * Call in ordinary task context with no existing CPU/dispatch lock. A short
 * native CPU lock protects the profile/pending-request check and reset.
 * Poll does nothing while inactive. Profile drift or a pending shutdown
 * disables further refresh; no pending flag is cleared or cancelled.
 * Refreshes is cumulative; last_error stores a signed result's u32 bits. */
int d88_power_idle_begin(void);
int d88_power_idle_poll(void);
/* Stop refreshing. The native timer resumes from its most recent reset;
 * elapsed idle time before Begin is deliberately not replayed on shutdown. */
void d88_power_idle_end(void);

#endif
