#ifndef D88_RESIDENT_RUNTIME_H
#define D88_RESIDENT_RUNTIME_H

typedef unsigned int d88_word;
#define D88_CONTROL_MAGIC 0x33443838u
#define D88_CONTROL_VERSION 1u
#define D88_TASK_ID 8u
#define D88_TASK_PRIORITY 7u
#define D88_STACK_BYTES 8192u

enum d88_service_state {
    D88_NEW=0, D88_STARTING=1, D88_RUNNING=2, D88_STOPPING=3,
    D88_WAIT_USB=4, D88_STOPPED=5, D88_FAILED=6
};

/* All fields are 32-bit for launcher inspection; no pointers into a previous
 * download application may be retained by this structure or the worker. */
typedef struct d88_control {
    d88_word magic, version, structure_bytes, image_id;
    d88_word self_base, allocation_bytes, allocation_raw;
    d88_word task_id, task_created, state, last_error, stop_requested;
    d88_word starts, polls, worker_address, start_address, stop_address;
} d88_control;

typedef struct d88_startup {
    d88_word self_base, allocation_bytes, allocation_raw;
    d88_word ros, gui, fs, crtl, audio, dict;
} d88_startup;

extern volatile d88_control resident_control;
int resident_entry(const d88_startup *configuration);
int resident_start(void);
void resident_request_stop(void);
void resident_worker(void);

#endif
