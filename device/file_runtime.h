#ifndef D88_FILE_RUNTIME_H
#define D88_FILE_RUNTIME_H

typedef struct d88_files_diagnostic {
    unsigned version,enabled,last_error,shutdown_requested;
} d88_files_diagnostic;
extern volatile d88_files_diagnostic files_diagnostic;
int d88_files_start(void);
int d88_files_poll(void);
void d88_files_reset(void *context);
int d88_files_rpc(void *context,unsigned opcode,const unsigned char *input,unsigned length,
                  unsigned char *output,unsigned *output_length);
/* Returns1 while GUI cleanup/message delivery is pending,0 once hook removed,
 * negative on an ownership/profile fault. No FS executes in the worker. */
int d88_files_shutdown_poll(void);
/* Trusted launcher GUI1 progress during its synchronous STOP wait. */
int d88_files_gui_progress(void);

#endif
