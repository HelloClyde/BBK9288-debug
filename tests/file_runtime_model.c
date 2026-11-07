/* Production orchestration with instrumented GUI/FS boundaries. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../device/file_service.h"
#include "../device/gui_jobs.h"
#include "../device/file_runtime.c"
static int available=1,needs,pending,bridge_idle=1,request_result,poll_result,stop_result,start_result;
static unsigned requests,polls,cancels,stops,inlines,steps,submits;
static int submit_result,file_poll_result;
static int fatal_error,cleanup_error;
static void (*installed)(void);
static d88_file_reply model_reply;
void d88_file_init(void) {}
int d88_file_available(void) {return available;}
int d88_file_submit(unsigned op,const unsigned char *data,unsigned length,unsigned *id)
{assert(op>=11u && op<=18u);assert(data || !length);submits++;*id=9;return submit_result;}
int d88_file_poll(unsigned id,d88_file_reply *out)
{assert(id==9u);*out=model_reply;return file_poll_result;}
void d88_file_cancel(void) {cancels++;}
void d88_file_gui_step(void) {steps++;needs=pending=0;bridge_idle=1;}
int d88_file_needs_step(void) {return needs;}
int d88_file_pending(void) {return pending;}
int d88_file_fatal_error(void) {return fatal_error;}
int d88_file_cleanup_error(void) {return cleanup_error;}
int d88_gui_jobs_start(void (*step)(void)) {installed=step;return start_result;}
int d88_gui_jobs_request(void) {requests++;return request_result;}
int d88_gui_jobs_poll(void) {polls++;return poll_result;}
int d88_gui_jobs_idle(void) {return bridge_idle;}
int d88_gui_jobs_stop(void) {stops++;return stop_result;}
int d88_gui_jobs_run_inline_one(void) {inlines++;assert(installed);installed();return 0;}
int main(int argc,char **argv)
{
 unsigned char input[4]={9,0,0,0},output[512];unsigned n=0;
 int which=argc>1?atoi(argv[1]):0;
 model_reply.job_id=9;model_reply.state=D88_FILE_RUNNING;
 if(which==0) {
  available=0;assert(d88_files_start()==-802);assert(!files_diagnostic.enabled);
  available=1;start_result=-5;assert(d88_files_start()==-815);assert(!files_diagnostic.enabled);
  start_result=0;assert(!d88_files_start());assert(installed==d88_file_gui_step);
  assert(d88_files_start()==-801);
 } else {
  assert(!d88_files_start());
  if(which==1) {
   needs=1;request_result=poll_result=D88_GUI_JOBS_BUSY;
   assert(!d88_files_poll());assert(requests==1u && polls==1u && !steps);
   request_result=-5;assert(d88_files_poll()==-835);assert(polls==1u);
   request_result=0;poll_result=-3;assert(d88_files_poll()==-853);
  } else if(which==2) {
   assert(!d88_files_rpc(0,15,input,4,output,&n));
   assert(n==8u && get32(output)==9u && !get32(output+4));
   assert(submits==1u && !steps);
   submit_result=-702;assert(!d88_files_rpc(0,15,input,4,output,&n));
   assert(n==8u && !get32(output) && (int)get32(output+4)==-702);
  } else if(which==3) {
   model_reply.length=496;memset(model_reply.data,0x5a,496);
   assert(!d88_files_rpc(0,19,input,4,output,&n));assert(n==512u);
   assert(get32(output)==9 && get32(output+4)==D88_FILE_RUNNING);
   assert(get32(output+12)==496u && output[511]==0x5a);
   model_reply.length=497;assert(d88_files_rpc(0,19,input,4,output,&n)==-3 && !n);
   assert(d88_files_rpc(0,19,input,3,output,&n)==-1);
   file_poll_result=-707;assert(d88_files_rpc(0,19,input,4,output,&n)==-1);
  } else if(which==4) {
   assert(!d88_files_rpc(0,20,input,4,output,&n));assert(cancels==1u && !n);
   model_reply.state=D88_FILE_DONE;
   assert(!d88_files_rpc(0,20,input,4,output,&n));assert(cancels==1u);
   d88_files_reset(0);assert(cancels==2u);
  } else if(which==5) {
   needs=pending=1;bridge_idle=0;
   assert(d88_files_shutdown_poll()==1);assert(cancels==1u && !stops && !steps);
   assert(d88_files_rpc(0,19,input,4,output,&n)==-2);
   assert(d88_files_shutdown_poll()==1);assert(cancels==1u);
   assert(!d88_files_gui_progress());assert(inlines==1u && steps==1u);
   stop_result=D88_GUI_JOBS_BUSY;assert(d88_files_shutdown_poll()==1);
   stop_result=0;assert(!d88_files_shutdown_poll());assert(!files_diagnostic.enabled);
   assert(!d88_files_shutdown_poll());d88_files_reset(0);assert(cancels==1u);
  } else if(which==6) {
   stop_result=-5;assert(d88_files_shutdown_poll()==-875);assert(files_diagnostic.enabled);
   assert((int)files_diagnostic.last_error==-875);
  } else if(which==7) {
   cleanup_error=-714;
   assert(!d88_files_poll()); /* A host can still fetch the recovery payload. */
   assert(!d88_files_rpc(0,19,input,4,output,&n));
   assert(d88_files_shutdown_poll()==-714);assert(files_diagnostic.enabled && !stops);
   assert((int)files_diagnostic.last_error==-714);
  } else if(which==8) {
   fatal_error=-709;
   assert(d88_files_poll()==-709);assert(!requests && !polls);
   assert(d88_files_shutdown_poll()==-709);assert(files_diagnostic.enabled && !stops);
  } else assert(0);
 }
 puts("PASS");return 0;
}
