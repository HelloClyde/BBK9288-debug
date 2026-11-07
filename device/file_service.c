/* One GUI-owned FS transaction; worker-side operations only exchange metadata. */
#include "file_service.h"
#include "file_native.h"
typedef unsigned char byte;
enum { TX_NONE,TX_GET,TX_PUT };
enum {
 P_START,P_GET_OPEN,P_GET_SIZE,P_GET_REWIND,P_GET_READ,P_GET_FINISH,
 P_PUT_CHECK,P_NAME_CHECK,P_TEMP_OPEN,P_PUT_WRITE,P_PUT_CLOSE,
 P_VERIFY_OPEN,P_VERIFY_SIZE,P_VERIFY_REWIND,P_VERIFY_READ,P_VERIFY_EOF,P_VERIFY_CLOSE,
 P_COMMIT_CHECK,P_COMMIT_BACKUP_CHECK,P_BACKUP_CHECK,P_MOVE,P_MOVE_FROM,P_MOVE_TO,P_BACKUP_REMOVE,
 P_CLEAN_CLOSE,P_CLEAN_TEMP,P_CLEAN_REMOVE,P_CLEAN_DONE
};
enum { MOVE_BACKUP=1,MOVE_PROMOTE,MOVE_UNPROMOTE,MOVE_RESTORE };
static d88_file_reply reply;
/* Native verification is independent of the480-byte transport framing. One
 * bounded16KiB read per GUI step keeps a16MiB FINISH within the job deadline. */
#define VERIFY_BYTES 16384u
static byte request[512],buffer[VERIFY_BYTES];
static unsigned request_bytes,request_opcode,next_job=1u,next_token=1u;
static volatile unsigned cancelled,active,phase;
static volatile int fatal_error,cleanup_error;
static struct {
 unsigned token,size,expected_crc,offset,crc,flags,existed,tries;
 unsigned temp_owned,backup_owned,target_owned,verified,verify_final,cancel_cleanup;
 unsigned verify_offset,verify_crc,move_kind,move_from_exists;
 int move_result,ending_result;
 void *stream;
 char target[D88_FILE_PATH_MAX+1u],temp[D88_FILE_PATH_MAX+1u],backup[D88_FILE_PATH_MAX+1u];
} tx;
static unsigned get32(const byte *p)
{return (unsigned)p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);}
static void put32(byte *p,unsigned v)
{p[0]=(byte)v;p[1]=(byte)(v>>8);p[2]=(byte)(v>>16);p[3]=(byte)(v>>24);}
static void zero(void *p,unsigned n) {byte *z=(byte*)p;while(n--) *z++=0;}
static void copy(void *p,const void *s,unsigned n)
{byte *d=(byte*)p;const byte *b=(const byte*)s;while(n--) *d++=*b++;}
static unsigned length(const char *p) {unsigned n=0;while(p[n]) n++;return n;}
static int equal(const char *a,const char *b)
{
 while(*a && *b) {
  unsigned x=(byte)*a++,y=(byte)*b++;
  if(x>='a' && x<='z') x-=32u;
  if(y>='a' && y<='z') y-=32u;
  if(x!=y) return 0;
 }
 return *a==*b;
}
static unsigned crc_update(unsigned c,const byte *p,unsigned n)
{unsigned i;while(n--) {c^=*p++;for(i=0;i<8u;i++) c=(c>>1)^(0xedb88320u&(0u-(c&1u)));}return c;}
/* Validate DBCS pairs before looking for separators: a GBK trail 5C is data. */
static int path_copy(char *out,const byte *p,unsigned n)
{
 unsigned i,start=3u,component=0;
 if(n<3u || n>D88_FILE_PATH_MAX || p[0]!='A' || p[1]!=':' || p[2]!='\\') return 0;
 for(i=3u;i<n;i++) {
  unsigned c=p[i];
  if(c>=0x81u && c<=0xfeu) {
   if(i+1u>=n || p[i+1u]<0x40u || p[i+1u]>0xfeu || p[i+1u]==0x7fu) return 0;
   i++;component++;continue;
  }
  if(c=='\\') {
   if(!component || p[i-1u]==' ' || p[i-1u]=='.' ||
      (i-start==1u && p[start]=='.') ||
      (i-start==2u && p[start]=='.' && p[start+1u]=='.')) return 0;
   start=i+1u;component=0;continue;
  }
  if(c<0x20u || c>=0x7fu || c==':' || c=='/' || c=='*' || c=='?' ||
     c=='"' || c=='<' || c=='>' || c=='|') return 0;
  component++;
 }
 if(n>3u && (!component || p[n-1u]==' ' || p[n-1u]=='.' ||
    (n-start==1u && p[start]=='.') ||
    (n-start==2u && p[start]=='.' && p[start+1u]=='.'))) return 0;
 copy(out,p,n);out[n]=0;return 1;
}
static int file_path(char *out,const byte *p,unsigned n)
{return path_copy(out,p,n) && n>3u;}
static void set_done(int result,unsigned n)
{
 if(d88_file_native_lock()!=0) {
  phase=P_START;
  if(active) fatal_error=D88_FILE_E_PROFILE;
  reply.result=D88_FILE_E_PROFILE;reply.length=0;reply.state=D88_FILE_DONE;return;
 }
 /* A native GUI FS call may block while the worker handles DTR/STOP. Do not
  * erase that single cancellation when the call returns successfully. Keep
  * this same job RUNNING so GUI cleanup remains schedulable without another
  * host command or a second cancel signal. Terminal cleanup clears active;
  * final PUT commit explicitly clears it only after commit has truly won. */
 if(cancelled && active && !fatal_error) {
  tx.cancel_cleanup=1u;tx.ending_result=D88_FILE_E_CANCELLED;
  phase=P_CLEAN_CLOSE;reply.result=0;reply.length=0;reply.state=D88_FILE_RUNNING;
 } else {
  phase=P_START;reply.result=result;reply.length=n;reply.state=D88_FILE_DONE;
  cancelled=0;
 }
 (void)d88_file_native_unlock();
}
static void result_pair(unsigned a,unsigned b)
{put32(reply.data,a);put32(reply.data+4u,b);}
static void result_triple(unsigned a,unsigned b,unsigned c)
{result_pair(a,b);put32(reply.data+8u,c);}
static void success_finished(void)
{
 /* A completed download has no committed mutation that can win cancellation.
  * In particular, its final native close may have blocked across DTR/STOP. */
 if(active==TX_GET && cancelled) {
  active=TX_NONE;set_done(D88_FILE_E_CANCELLED,0);return;
 }
 result_triple(tx.token,tx.size,~tx.crc);active=TX_NONE;
 set_done(D88_FILE_OK,12u);
}
/* Error payload is recovery metadata only: token,temp_len,temp,backup_len,backup.
 * Never delete an ambiguous destination or a possibly sole original backup. */
static void recovery(void)
{
 unsigned a=length(tx.temp),b=length(tx.backup),at=8u;
 put32(reply.data,tx.token);put32(reply.data+4u,a);copy(reply.data+at,tx.temp,a);at+=a;
 put32(reply.data+at,b);at+=4u;copy(reply.data+at,tx.backup,b);at+=b;
 cleanup_error=D88_FILE_E_RECOVERY;
 active=TX_NONE;tx.stream=0;set_done(D88_FILE_E_RECOVERY,at);
}
static int close_stream(void)
{
 void *s=tx.stream;int r;
 tx.stream=0;r=d88_file_native_close(s);
 if(r && (cancelled || tx.cancel_cleanup)) cleanup_error=D88_FILE_E_IO;
 return r;
}
static void start_cleanup(int result)
{tx.ending_result=result;phase=P_CLEAN_CLOSE;}
static void start_move(unsigned kind)
{tx.move_kind=kind;phase=P_MOVE;}
static const char *move_from(void)
{
 if(tx.move_kind==MOVE_BACKUP) return tx.target;
 if(tx.move_kind==MOVE_PROMOTE) return tx.temp;
 if(tx.move_kind==MOVE_UNPROMOTE) return tx.target;
 return tx.backup;
}
static const char *move_to(void)
{
 if(tx.move_kind==MOVE_BACKUP) return tx.backup;
 if(tx.move_kind==MOVE_UNPROMOTE) return tx.temp;
 return tx.target;
}
/* Called only after both post-rename names have been inspected. */
static void moved(void)
{
 unsigned kind=tx.move_kind;
 if(kind==MOVE_BACKUP) tx.backup_owned=1u;
 if(kind==MOVE_PROMOTE) {tx.temp_owned=0;tx.target_owned=1u;}
 if(kind==MOVE_UNPROMOTE) {tx.temp_owned=1u;tx.target_owned=0;}
 if(kind==MOVE_RESTORE) tx.backup_owned=0;
 if(tx.move_result!=0) {
  if(kind==MOVE_UNPROMOTE || kind==MOVE_RESTORE) {recovery();return;}
  start_cleanup(D88_FILE_E_COMMIT);return;
 }
 if(kind==MOVE_BACKUP) start_move(MOVE_PROMOTE);
 else if(kind==MOVE_PROMOTE) {tx.verify_final=1u;phase=P_VERIFY_OPEN;}
 else if(kind==MOVE_UNPROMOTE) start_move(MOVE_RESTORE);
 else phase=P_CLEAN_TEMP;
}
static void parse_job(void)
{
 unsigned op=request_opcode,n=request_bytes,token;
 if(op==D88_FILE_LIST_OPEN || op==D88_FILE_LIST_NEXT) {set_done(D88_FILE_E_UNSUPPORTED,0);return;}
 if(op==D88_FILE_ABORT) {
  if(n!=4u) {set_done(D88_FILE_E_ARGUMENT,0);return;}
  token=get32(request);
  if(active && token && token!=tx.token) {set_done(D88_FILE_E_STATE,0);return;}
  if(!active) {set_done(0,0);return;}
  tx.cancel_cleanup=1u;
  start_cleanup(0);return;
 }
 if(op==D88_FILE_GET_OPEN || op==D88_FILE_PUT_OPEN) {
  unsigned head=op==D88_FILE_GET_OPEN?4u:16u,path_n;
  if(active) {set_done(D88_FILE_E_BUSY,0);return;}
  if(n<head) {set_done(D88_FILE_E_ARGUMENT,0);return;}
  path_n=get32(request+head-4u);
  if(path_n!=n-head) {set_done(D88_FILE_E_ARGUMENT,0);return;}
  zero(&tx,sizeof(tx));
  if(!file_path(tx.target,request+head,path_n)) {set_done(D88_FILE_E_PATH,0);return;}
  if(!next_token || next_token>0xffffffu) {set_done(D88_FILE_E_LIMIT,0);return;}
  tx.token=next_token++;tx.crc=~0u;
  if(op==D88_FILE_GET_OPEN) {active=TX_GET;phase=P_GET_OPEN;return;}
  tx.size=get32(request);tx.expected_crc=get32(request+4u);tx.flags=get32(request+8u);
  if(tx.size>D88_FILE_MAX_BYTES || (tx.flags&~D88_FILE_PUT_REPLACE)) {
   set_done(D88_FILE_E_ARGUMENT,0);return;
  }
  active=TX_PUT;phase=P_PUT_CHECK;return;
 }
 if(n<4u || !active || get32(request)!=tx.token) {set_done(D88_FILE_E_STATE,0);return;}
 if(op==D88_FILE_GET_READ) {
  if(active!=TX_GET || n!=12u || get32(request+4u)!=tx.offset ||
     !get32(request+8u) || get32(request+8u)>D88_FILE_CHUNK) {set_done(D88_FILE_E_ARGUMENT,0);return;}
  phase=P_GET_READ;return;
 }
 if(op==D88_FILE_PUT_DATA) {
  unsigned size=n-12u;
  if(active!=TX_PUT || n<=12u || size>D88_FILE_CHUNK ||
     get32(request+4u)!=tx.offset || tx.offset>tx.size || size>tx.size-tx.offset) {
   set_done(D88_FILE_E_ARGUMENT,0);return;
  }
  if(~crc_update(~0u,request+12u,size)!=get32(request+8u)) {set_done(D88_FILE_E_CRC,0);return;}
  phase=P_PUT_WRITE;return;
 }
 if(op==D88_FILE_FINISH) {
  if(n!=4u || tx.offset!=tx.size) {set_done(D88_FILE_E_SIZE,0);return;}
  if(active==TX_GET) {phase=P_GET_FINISH;return;}
  if(~tx.crc!=tx.expected_crc) {start_cleanup(D88_FILE_E_CRC);return;}
  phase=P_PUT_CLOSE;return;
 }
 set_done(D88_FILE_E_ARGUMENT,0);
}
static int owned_names(void)
{
 unsigned i,dir=3u,j,v=tx.token+tx.tries;
 static const char hex[]="0123456789ABCDEF";
 for(i=3u;tx.target[i];i++) {
  unsigned c=(byte)tx.target[i];
  if(c>=0x81u && c<=0xfeu) {i++;continue;}
  if(c=='\\') dir=i+1u;
 }
 if(dir+12u>D88_FILE_PATH_MAX) return 0;
 copy(tx.temp,tx.target,dir);copy(tx.backup,tx.target,dir);
 tx.temp[dir]='~';tx.temp[dir+1u]='D';
 for(j=0;j<6u;j++) tx.temp[dir+2u+j]=hex[(v>>(20u-4u*j))&15u];
 copy(tx.temp+dir+8u,".TMP",5u);copy(tx.backup+dir,tx.temp+dir,8u);
 copy(tx.backup+dir+8u,".BAK",5u);
 return !equal(tx.target,tx.temp) && !equal(tx.target,tx.backup);
}
void d88_file_gui_step(void)
{
 int r;unsigned n,wanted;
 if(!d88_file_needs_step()) return;
 if(fatal_error) return;
 if(!d88_file_native_ready()) {
  /* Preserve handles/owned names on profile loss. A future verified GUI step
   * can clean them; STOP must retain this image while pending remains true. */
  if(active) fatal_error=D88_FILE_E_PROFILE;
  set_done(D88_FILE_E_PROFILE,0);
  return;
 }
 if(reply.state==D88_FILE_QUEUED) reply.state=D88_FILE_RUNNING;
 if(cancelled) tx.cancel_cleanup=1u;
 if(cancelled && phase!=P_CLEAN_CLOSE && phase!=P_CLEAN_TEMP && phase!=P_CLEAN_REMOVE && phase!=P_CLEAN_DONE &&
    !(phase>=P_MOVE && phase<=P_MOVE_TO)) start_cleanup(D88_FILE_E_CANCELLED);
 if(!active && cancelled) {set_done(D88_FILE_E_CANCELLED,0);return;}
 switch(phase) {
 case P_START:parse_job();break;
 case P_GET_OPEN:
  tx.stream=d88_file_native_open(tx.target,"rb");
  if(!tx.stream) {
   active=0;set_done(d88_file_native_errno()==0x12u?D88_FILE_E_NOT_FOUND:D88_FILE_E_IO,0);
  } else phase=P_GET_SIZE;
  break;
 case P_GET_SIZE:
  r=d88_file_native_seek(tx.stream,0,2u);
  if(r<0 || (unsigned)r>D88_FILE_MAX_BYTES || d88_file_native_tell(tx.stream)!=r)
   start_cleanup(r>=0?D88_FILE_E_SIZE:D88_FILE_E_IO);
  else {tx.size=(unsigned)r;phase=P_GET_REWIND;}
  break;
 case P_GET_REWIND:
  if(d88_file_native_seek(tx.stream,0,0)!=0 || d88_file_native_tell(tx.stream)!=0) start_cleanup(D88_FILE_E_IO);
  else {result_pair(tx.token,tx.size);set_done(0,8u);}
  break;
 case P_GET_READ:
  wanted=get32(request+8u);if(wanted>tx.size-tx.offset) wanted=tx.size-tx.offset;
  n=d88_file_native_read(tx.stream,reply.data+12u,wanted);
  if(n!=wanted || d88_file_native_error(tx.stream)) {start_cleanup(D88_FILE_E_IO);break;}
  result_triple(tx.token,tx.offset,~crc_update(~0u,reply.data+12u,n));
  tx.crc=crc_update(tx.crc,reply.data+12u,n);tx.offset+=n;set_done(0,12u+n);break;
 case P_GET_FINISH:
  r=close_stream();
  if(r) {active=0;set_done(D88_FILE_E_IO,0);}
  else {
   /* A second closed/reopened pass detects file changes during the download. */
   tx.expected_crc=~tx.crc;tx.verify_final=1u;phase=P_VERIFY_OPEN;
  }
  break;
 case P_PUT_CHECK:
  r=d88_file_native_exists(tx.target);
  if(r<0) start_cleanup(D88_FILE_E_IO);
  else if(r && !(tx.flags&D88_FILE_PUT_REPLACE)) start_cleanup(D88_FILE_E_EXISTS);
  else {tx.existed=(unsigned)r;phase=P_NAME_CHECK;}
  break;
 case P_NAME_CHECK:
  if(tx.tries>=32u || !owned_names()) {start_cleanup(D88_FILE_E_LIMIT);break;}
  r=d88_file_native_exists(tx.temp);
  if(r<0) start_cleanup(D88_FILE_E_IO);
  else if(r) tx.tries++;
  else phase=P_BACKUP_CHECK;
  break;
 case P_BACKUP_CHECK:
  r=d88_file_native_exists(tx.backup);
  if(r<0) start_cleanup(D88_FILE_E_IO);
  else if(r) {tx.tries++;phase=P_NAME_CHECK;}
  else phase=P_TEMP_OPEN;
  break;
 case P_TEMP_OPEN:
  /* Absence was checked in this GUI-only transaction. This firmware offers no
   * O_EXCL; the ~D names are reserved for this service and never reused blindly. */
  r=d88_file_native_exists(tx.temp);
  if(r<0) {start_cleanup(D88_FILE_E_IO);break;}
  if(r) {tx.tries++;phase=P_NAME_CHECK;break;}
  tx.temp_owned=1u;tx.stream=d88_file_native_open(tx.temp,"wb");
  if(!tx.stream) start_cleanup(D88_FILE_E_IO);
  else {result_pair(tx.token,0);set_done(0,8u);}break;
 case P_PUT_WRITE:
  n=request_bytes-12u;r=(int)d88_file_native_write(tx.stream,request+12u,n);
  if(r!=(int)n || d88_file_native_error(tx.stream)) {start_cleanup(D88_FILE_E_IO);break;}
  tx.crc=crc_update(tx.crc,request+12u,n);tx.offset+=n;
  result_triple(tx.token,tx.offset,~tx.crc);set_done(0,12u);break;
 case P_PUT_CLOSE:
  r=close_stream();
  if(r) start_cleanup(D88_FILE_E_IO);
  else {tx.verify_final=0;phase=P_VERIFY_OPEN;}break;
 case P_VERIFY_OPEN:
  tx.stream=d88_file_native_open(tx.verify_final?tx.target:tx.temp,"rb");
  if(!tx.stream) start_cleanup(D88_FILE_E_IO);else phase=P_VERIFY_SIZE;break;
 case P_VERIFY_SIZE:
  r=d88_file_native_seek(tx.stream,0,2u);
  if(r!=(int)tx.size || d88_file_native_tell(tx.stream)!=r) start_cleanup(D88_FILE_E_SIZE);
  else phase=P_VERIFY_REWIND;
  break;
 case P_VERIFY_REWIND:
  r=d88_file_native_seek(tx.stream,0,0);
  if(r || d88_file_native_tell(tx.stream)!=0) start_cleanup(D88_FILE_E_IO);
  else {tx.verify_offset=0;tx.verify_crc=~0u;phase=P_VERIFY_READ;}break;
 case P_VERIFY_READ:
  wanted=tx.size-tx.verify_offset;if(wanted>VERIFY_BYTES) wanted=VERIFY_BYTES;
  if(!wanted) {phase=P_VERIFY_EOF;break;}
  n=d88_file_native_read(tx.stream,buffer,wanted);
  if(n!=wanted || d88_file_native_error(tx.stream)) {start_cleanup(D88_FILE_E_IO);break;}
  tx.verify_crc=crc_update(tx.verify_crc,buffer,n);tx.verify_offset+=n;break;
 case P_VERIFY_EOF:
  n=d88_file_native_read(tx.stream,buffer,1u);
  if(n || d88_file_native_error(tx.stream)) start_cleanup(D88_FILE_E_SIZE);
  else if(~tx.verify_crc!=tx.expected_crc) start_cleanup(D88_FILE_E_CRC);
  else phase=P_VERIFY_CLOSE;
  break;
 case P_VERIFY_CLOSE:
  r=close_stream();
  if(r) {start_cleanup(D88_FILE_E_IO);break;}
  if(!tx.verify_final) phase=P_COMMIT_CHECK;
  else if(tx.backup_owned) {tx.verified=1u;phase=P_BACKUP_REMOVE;}
  else success_finished();
  break;
 case P_COMMIT_CHECK:
  r=d88_file_native_exists(tx.target);
  if(r<0) start_cleanup(D88_FILE_E_IO);
  else if((unsigned)r!=tx.existed) start_cleanup(D88_FILE_E_COMMIT);
  else if(r) phase=P_COMMIT_BACKUP_CHECK;
  else start_move(MOVE_PROMOTE);
  break;
 case P_COMMIT_BACKUP_CHECK:
  r=d88_file_native_exists(tx.backup);
  if(r<0) start_cleanup(D88_FILE_E_IO);
  else if(r) start_cleanup(D88_FILE_E_COMMIT);
  else start_move(MOVE_BACKUP);
  break;
 case P_MOVE:
  tx.move_result=d88_file_native_rename(move_from(),move_to());phase=P_MOVE_FROM;break;
 case P_MOVE_FROM:
  r=d88_file_native_exists(move_from());
  if(r<0) recovery();else {tx.move_from_exists=(unsigned)r;phase=P_MOVE_TO;}break;
 case P_MOVE_TO:
  r=d88_file_native_exists(move_to());
  if(r<0 || r==(int)tx.move_from_exists) {recovery();break;}
  if(!tx.move_from_exists && r) {moved();break;}
  /* Definitely unchanged; do not retry the rename. */
  if(tx.move_kind==MOVE_UNPROMOTE || tx.move_kind==MOVE_RESTORE) recovery();
  else start_cleanup(D88_FILE_E_COMMIT);
  break;
 case P_BACKUP_REMOVE:
  r=d88_file_native_remove(tx.backup);
  if(r) recovery();else {tx.backup_owned=0;success_finished();}break;
 case P_CLEAN_CLOSE:
  if(tx.stream) {
   if(close_stream()) tx.ending_result=D88_FILE_E_IO;
   break;
  }
  if(tx.backup_owned) {
   if(tx.target_owned) start_move(MOVE_UNPROMOTE);else start_move(MOVE_RESTORE);
  } else if(tx.target_owned) recovery();
  else phase=P_CLEAN_TEMP;
  break;
 case P_CLEAN_TEMP:
  if(tx.temp_owned) {
   r=d88_file_native_exists(tx.temp);
   if(r<0) {recovery();break;}
   if(r) {phase=P_CLEAN_REMOVE;break;}
   tx.temp_owned=0;
  }
  phase=P_CLEAN_DONE;break;
 case P_CLEAN_REMOVE:
  tx.temp_owned=0;
  if(d88_file_native_remove(tx.temp)) recovery();else phase=P_CLEAN_DONE;
  break;
 case P_CLEAN_DONE:
  active=0;set_done(tx.ending_result,0);break;
 default:recovery();break;
 }
}
int d88_file_available(void) {return d88_file_native_available();}
int d88_file_fatal_error(void) {return fatal_error;}
int d88_file_cleanup_error(void) {return cleanup_error;}
void d88_file_init(void)
{
 /* Resident image is cold-zeroed. Refuse destructive reinitialization. */
 if(fatal_error || cleanup_error || active || reply.state==D88_FILE_QUEUED || reply.state==D88_FILE_RUNNING) return;
 zero(&reply,sizeof(reply));zero(&tx,sizeof(tx));phase=P_START;cancelled=0;
}
int d88_file_submit(unsigned op,const byte *payload,unsigned bytes,unsigned *id)
{
 int r=0;
 if(id) *id=0;
 if(!id || !payload || bytes>sizeof(request) || op<D88_FILE_LIST_OPEN || op>D88_FILE_ABORT)
  return D88_FILE_E_ARGUMENT;
 if(fatal_error) return fatal_error;
 if(cleanup_error) return cleanup_error;
 if(!d88_file_available()) {if(active) fatal_error=D88_FILE_E_PROFILE;return D88_FILE_E_PROFILE;}
 if(d88_file_native_lock()!=0) {if(active) fatal_error=D88_FILE_E_PROFILE;return D88_FILE_E_PROFILE;}
 if(reply.state==D88_FILE_QUEUED || reply.state==D88_FILE_RUNNING || cancelled) r=D88_FILE_E_BUSY;
 else if(!next_job) r=D88_FILE_E_LIMIT;
 else {
  copy(request,payload,bytes);request_bytes=bytes;request_opcode=op;phase=P_START;
  reply.job_id=next_job++;reply.result=0;reply.length=0;reply.state=D88_FILE_QUEUED;
  *id=reply.job_id;
 }
 if(d88_file_native_unlock()!=0) return D88_FILE_E_PROFILE;
 return r;
}
int d88_file_poll(unsigned id,d88_file_reply *out)
{
 int r=0;
 if(!out || !id) return D88_FILE_E_ARGUMENT;
 if(d88_file_native_lock()!=0) {if(active) fatal_error=D88_FILE_E_PROFILE;return D88_FILE_E_PROFILE;}
 if(id!=reply.job_id) r=D88_FILE_E_STATE;else copy(out,&reply,sizeof(reply));
 if(d88_file_native_unlock()!=0) return D88_FILE_E_PROFILE;
 return r;
}
void d88_file_cancel(void)
{
 if(d88_file_native_lock()!=0) {if(active) fatal_error=D88_FILE_E_PROFILE;return;}
 if(active || reply.state==D88_FILE_QUEUED || reply.state==D88_FILE_RUNNING) {
  cancelled=1u;
  if(reply.state==D88_FILE_DONE) {reply.state=D88_FILE_RUNNING;reply.length=0;}
 }
 (void)d88_file_native_unlock();
}
int d88_file_needs_step(void)
{return reply.state==D88_FILE_QUEUED || reply.state==D88_FILE_RUNNING || (cancelled && active);}
int d88_file_pending(void) {return active || d88_file_needs_step();}
