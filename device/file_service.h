#ifndef D88_FILE_SERVICE_H
#define D88_FILE_SERVICE_H
#include "file_protocol.h"
typedef struct d88_file_reply {
    unsigned job_id;
    volatile unsigned state; /* Published last; GUI and worker share this word. */
    int result;
    unsigned length;
    unsigned char data[D88_FILE_MAX_REPLY];
} d88_file_reply;
/* Submit/poll/cancel are bounded metadata operations; never call native FS. */
int d88_file_submit(unsigned opcode,const unsigned char *payload,unsigned bytes,unsigned *job_id);
int d88_file_poll(unsigned job_id,d88_file_reply *reply);
void d88_file_cancel(void);
/* The GUI bridge calls step only in the verified GUI1 ordinary task context. */
void d88_file_gui_step(void);
int d88_file_needs_step(void);
int d88_file_pending(void);
int d88_file_available(void);
/* Latched for this retained image. Profile loss while owning a transaction is
 * fatal; cleanup/recovery errors remain pollable until shutdown. Neither an
 * ordinary file error nor a successful re-init clears this evidence. */
int d88_file_fatal_error(void);
int d88_file_cleanup_error(void);
/* Cold initialization only. Never discard a live transaction to initialize. */
void d88_file_init(void);
#endif
