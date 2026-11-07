#ifndef D88_APP_REFRESH_H
#define D88_APP_REFRESH_H

/* Native scanner execution is a GUI1 job, never a USB-worker operation.
 * available() only reads the captured profile and initialized FS state.
 * execute() requires the owned unlocked GUI callback and idle home desktop.
 * A completed scan cannot be rolled back; post_result and call counts retain
 * partial progress on failure and callers must not automatically replay it.
 */
typedef struct {
    unsigned before_total, after_total, before_cat8, after_cat8;
    unsigned scanner_calls, post_calls, post_result, home;
} d88_app_refresh_result;

int d88_app_refresh_available(void);
/* Returns an existing D88_FILE_E_* code or zero. Does not acquire ROS locks. */
int d88_app_refresh_execute(d88_app_refresh_result *result);

#endif
