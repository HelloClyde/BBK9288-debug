#ifndef D88_GUI_JOBS_H
#define D88_GUI_JOBS_H

/* Captured-profile desktop bridge. All functions require ordinary task context.
 * start() installs only into an empty native desktop hook slot. The supplied
 * function and this bridge must live in the retained resident image.
 * request() marks one GUI step; poll() posts it from the worker. Neither calls
 * step synchronously. Repeated request() calls coalesce until that step begins.
 * The callback executes one step, outside all ROS locks, in native GUI task 1.
 * A step may request another step, but only a later worker poll can post it.
 * stop() requires idle() and restores only our own slot; callers must also have
 * closed their own file transactions. Never release the resident image. */
#define D88_GUI_JOBS_OK 0
#define D88_GUI_JOBS_BUSY (-2)
#define D88_GUI_JOBS_PROFILE (-3)
#define D88_GUI_JOBS_CONTEXT (-4)
#define D88_GUI_JOBS_OWNERSHIP (-5)
#define D88_GUI_JOBS_QUEUE (-6)
#define D88_GUI_JOBS_INACTIVE (-7)
#define D88_GUI_JOBS_EXHAUSTED (-8)
#define D88_GUI_JOBS_PRIVATE_MESSAGE 0x7f88u
#define D88_GUI_JOBS_COOKIE_MASK 0x4a4f4238u

int d88_gui_jobs_start(void (*step)(void));
int d88_gui_jobs_request(void);
int d88_gui_jobs_poll(void);
int d88_gui_jobs_idle(void);
int d88_gui_jobs_stop(void);
/* GUI1 launcher STOP waits cannot rely on their own message pump. Execute at
 * most one requested/pending step there, outside ROS locks. Existing scalar
 * queued messages become stale; the guarded native default path ignores them.
 * Returns 0 for success/no work, -2 for reentry, another negative on failure. */
int d88_gui_jobs_run_inline_one(void);
int d88_gui_jobs_context(void); /* 1 only in an owned, unlocked GUI job step. */
int d88_gui_jobs_last_error(void);

#endif
