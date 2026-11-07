#ifndef D88_INPUT_JOBS_H
#define D88_INPUT_JOBS_H
/* Native GUI messages; direct hardware key polling is outside this capability. */
int d88_input_available(void);
int d88_input_queue(unsigned scan);
int d88_input_poll(void);
int d88_input_pending(void);
int d88_input_last_error(void);
void d88_input_reset(void);
void d88_input_cancel(void); /* Drop unposted DOWN; keep UP after accepted DOWN. */
#endif
