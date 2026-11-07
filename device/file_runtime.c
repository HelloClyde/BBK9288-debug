#include "file_runtime.h"
#include "file_service.h"
#include "gui_jobs.h"

volatile d88_files_diagnostic files_diagnostic={1u,0,0,0};
static d88_file_reply reply;

static unsigned get32(const unsigned char *p)
{return (unsigned)p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);}
static void put32(unsigned char *p,unsigned n)
{p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);p[2]=(unsigned char)(n>>16);p[3]=(unsigned char)(n>>24);}
static int fault(int result)
{files_diagnostic.last_error=(unsigned)result;return result;}

int d88_files_start(void)
{
    int result;
    if(files_diagnostic.enabled) return -801;
    d88_file_init();
    result=d88_file_fatal_error();if(result) return fault(result);
    result=d88_file_cleanup_error();if(result) return fault(result);
    if(!d88_file_available()) return fault(-802);
    result=d88_gui_jobs_start(d88_file_gui_step);
    if(result) return fault(-810+result);
    files_diagnostic.version=1;files_diagnostic.enabled=1;
    files_diagnostic.last_error=files_diagnostic.shutdown_requested=0;
    return 0;
}
int d88_files_poll(void)
{
    int result;
    if(!files_diagnostic.enabled) return 0;
    result=d88_file_fatal_error();if(result) return fault(result);
    if(d88_file_needs_step()) {
        result=d88_gui_jobs_request();
        if(result && result!=D88_GUI_JOBS_BUSY) return fault(-830+result);
    }
    result=d88_gui_jobs_poll();
    if(result && result!=D88_GUI_JOBS_BUSY) return fault(-850+result);
    return 0;
}
void d88_files_reset(void *context)
{
    (void)context;
    if(files_diagnostic.enabled) d88_file_cancel();
}
int d88_files_rpc(void *context,unsigned opcode,const unsigned char *input,unsigned length,
                  unsigned char *output,unsigned *output_length)
{
    unsigned id,i;
    int result;
    (void)context;
    if(!output || !output_length || (!input && length)) return -1;
    *output_length=0;
    if(!files_diagnostic.enabled || files_diagnostic.shutdown_requested) return -2;
    if((opcode>=D88_FILE_LIST_OPEN && opcode<=D88_FILE_ABORT) || opcode==D88_APP_REFRESH) {
        id=0;result=d88_file_submit(opcode,input,length,&id);
        put32(output,result?0u:id);put32(output+4u,(unsigned)result);*output_length=8u;
        if(!result) (void)d88_files_poll();
        return 0;
    }
    if((opcode!=D88_FILE_POLL && opcode!=D88_FILE_CANCEL_JOB) || length!=4u) return -1;
    id=get32(input);
    result=d88_file_poll(id,&reply);
    if(result) return -1;
    if(opcode==D88_FILE_CANCEL_JOB) {
        if(reply.state!=D88_FILE_DONE) d88_file_cancel();
        (void)d88_files_poll();return 0;
    }
    if(reply.length>D88_FILE_MAX_REPLY) return -3;
    put32(output,reply.job_id);put32(output+4u,reply.state);
    put32(output+8u,(unsigned)reply.result);put32(output+12u,reply.length);
    for(i=0;i<reply.length;i++) output[16u+i]=reply.data[i];
    *output_length=16u+reply.length;
    return 0;
}
int d88_files_shutdown_poll(void)
{
    int result;
    if(!files_diagnostic.enabled) return 0;
    if(!files_diagnostic.shutdown_requested) {
        files_diagnostic.shutdown_requested=1;d88_file_cancel();
    }
    result=d88_files_poll();
    if(result) return result;
    result=d88_file_cleanup_error();if(result) return fault(result);
    if(d88_file_pending() || !d88_gui_jobs_idle()) return 1;
    result=d88_gui_jobs_stop();
    if(result==D88_GUI_JOBS_BUSY) return 1;
    if(result) return fault(-870+result);
    files_diagnostic.enabled=0;
    return 0;
}
int d88_files_gui_progress(void)
{
    if(!files_diagnostic.enabled) return 0;
    if(d88_file_needs_step()) (void)d88_gui_jobs_request();
    return d88_gui_jobs_run_inline_one();
}
