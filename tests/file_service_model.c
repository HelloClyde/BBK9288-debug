/* In-memory FAT/close/rename fault model for the actual production FSM. */
#ifdef D88_FILE_ADAPTER_MODEL
#include <assert.h>
#include <string.h>
#define D88_FILE_NATIVE_TEST 1
#include "../device/file_native.c"
static unsigned char adapter_ram[0x800000];
static unsigned adapter_gui,adapter_locks,adapter_stat_calls,adapter_error;
static unsigned adapter_psr,adapter_gp,adapter_corrupt_on_lock;
static unsigned adapter_directory;
static int adapter_stat_result;
static void adapter_write(unsigned a,unsigned v)
{
 unsigned char *p=adapter_ram+a-0x02000000u;
 p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);p[2]=(unsigned char)(v>>16);p[3]=(unsigned char)(v>>24);
}
unsigned char d88_fn_test_read8(unsigned a)
{assert(a>=0x02000000u && a<0x02800000u);return adapter_ram[a-0x02000000u];}
unsigned d88_fn_test_read32(unsigned a)
{return (unsigned)d88_fn_test_read8(a)|((unsigned)d88_fn_test_read8(a+1u)<<8)|((unsigned)d88_fn_test_read8(a+2u)<<16)|((unsigned)d88_fn_test_read8(a+3u)<<24);}
unsigned d88_fn_test_psr(void) {return adapter_psr;}
unsigned d88_fn_test_gp(void) {return adapter_gp;}
int d88_fn_test_lock(void)
{
 adapter_locks++;adapter_psr=0;adapter_ram[0x6866d]=3;
 if(adapter_corrupt_on_lock) adapter_write(adapter_corrupt_on_lock,0);
 return 0;
}
int d88_fn_test_unlock(void)
{assert(adapter_locks);adapter_locks--;adapter_psr=0x10u;adapter_ram[0x6866d]=0;return 0;}
int d88_gui_jobs_context(void) {return adapter_gui && adapter_psr==0x10u && !adapter_gp && !adapter_locks;}
int d88_fn_test_stat(const char *p,void *b)
{(void)p;assert(adapter_gui && !adapter_locks);((unsigned*)b)[0]=adapter_directory?0x40000000u:0u;adapter_stat_calls++;adapter_write(0x0205ac8cu,adapter_error);return adapter_stat_result;}
void adapter_reset(void)
{
 unsigned i,j;
 memset(adapter_ram,0,sizeof(adapter_ram));adapter_gui=1;adapter_locks=adapter_stat_calls=adapter_error=0;adapter_stat_result=0;
 adapter_psr=0x10u;adapter_gp=adapter_corrupt_on_lock=adapter_directory=0;
 for(i=0;i<6u;i++) adapter_write(0x02000200u+4u*i,roots[i]);
 for(i=0;i<29u;i++) adapter_write(0x021e7ae0u+4u*i,entries[i]);
 for(i=0;i<sizeof(signatures)/sizeof(signatures[0]);i++)
  for(j=0;j<signatures[i].bytes;j++) adapter_ram[signatures[i].address+j-0x02000000u]=signatures[i].code[j];
 adapter_write(0x0205ac84u,1u);adapter_write(0x021e8244u,0x021ce8aeu);adapter_write(0x021e8248u,0x021ce8d8u);
 adapter_write(0x022a9820u,0x02068b34u);adapter_write(0x022a9850u,0x02068b34u);
 adapter_write(0x02068664u,0x02068b34u+72u);
}
void adapter_corrupt(unsigned a) {adapter_ram[a-0x02000000u]^=1u;}
void adapter_context(unsigned v) {adapter_gui=v;}
void adapter_stat(int result,unsigned error) {adapter_stat_result=result;adapter_error=error;}
void adapter_dir(unsigned value) {adapter_directory=value;}
unsigned adapter_calls(void) {return adapter_stat_calls;}
void adapter_execution(unsigned psr_value,unsigned gp_value,unsigned depth,unsigned flags)
{adapter_psr=psr_value;adapter_gp=gp_value;adapter_ram[0x6866c]=(unsigned char)depth;adapter_ram[0x6866d]=(unsigned char)flags;}
void adapter_race(unsigned address) {adapter_corrupt_on_lock=address;}
unsigned adapter_lock_depth(void) {return adapter_locks;}
#else
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "../device/file_service.c"
#define FILES 32u
#define HANDLES 16u
#define CAPACITY 32768u
static struct {char name[241];byte data[CAPACITY];unsigned size,used,directory;} files[FILES];
static struct {unsigned live,index,offset,writable;} handles[HANDLES];
static unsigned profile_ok,in_gui,lock_depth,fs_calls,bad_calls,close_calls,rename_calls,remove_calls;
static unsigned fail_op,fail_nth,fail_mode,fail_seen,last_errno,cancel_remove,fail2_nth,fail2_mode;
static unsigned counts[16];
static unsigned max_read;
static unsigned cancel_call_op,cancel_call_nth,cancel_call_seen,cancel_call_injections;
enum { OP_OPEN=1,OP_READ,OP_WRITE,OP_CLOSE,OP_EXISTS,OP_RENAME,OP_REMOVE,OP_SEEK,OP_TELL };
static int same(const char *a,const char *b)
{
 while(*a && *b) {
  unsigned x=(byte)*a++,y=(byte)*b++;
  if(x>='a' && x<='z') x-=32u;
  if(y>='a' && y<='z') y-=32u;
  if(x!=y) return 0;
 }
 return *a==*b;
}
static int find(const char *p)
{unsigned i;for(i=0;i<FILES;i++) if(files[i].used && same(files[i].name,p)) return (int)i;return -1;}
static int empty(void) {unsigned i;for(i=0;i<FILES;i++) if(!files[i].used) return (int)i;return -1;}
static int call(unsigned op)
{
 assert(in_gui && !lock_depth);fs_calls++;counts[op]++;
 if(op==cancel_call_op && ++cancel_call_seen==cancel_call_nth) {
  /* Model the worker running while this unlocked native GUI FS call blocks.
   * It signals cancellation once, then the same FS call returns normally. */
  cancel_call_op=0;cancel_call_injections++;in_gui=0;
  d88_file_cancel();in_gui=1;assert(!lock_depth);
 }
 if(op==fail_op) {
  fail_seen++;
  if(fail_seen==fail_nth) return (int)fail_mode;
  if(fail_seen==fail2_nth) return (int)fail2_mode;
 }
 return 0;
}
static unsigned handle(void *p)
{
 uintptr_t n=(uintptr_t)p;
 if(n<1u || n>HANDLES || !handles[n-1u].live) {bad_calls++;assert(0);}
 return (unsigned)n-1u;
}
int d88_file_native_available(void) {return (int)profile_ok;}
int d88_file_native_ready(void) {return profile_ok && in_gui && !lock_depth;}
int d88_file_native_lock(void) {assert(!lock_depth);lock_depth=1;return 0;}
int d88_file_native_unlock(void) {assert(lock_depth==1u);lock_depth=0;return 0;}
void *d88_file_native_open(const char *p,const char *mode)
{
 unsigned i;int f=find(p),fail=call(OP_OPEN);
 if(fail==1) return 0;
 if(f>=0 && files[f].directory) {last_errno=0x12u;return 0;}
 if(mode[0]=='w') {
  if(f<0) f=empty();
  assert(f>=0);files[f].used=1;strcpy(files[f].name,p);files[f].size=0;
 } else if(f<0) {last_errno=0x12u;return 0;}
 if(fail==2) return 0;
 for(i=0;i<HANDLES;i++) if(!handles[i].live) {
  handles[i].live=1;handles[i].index=(unsigned)f;handles[i].offset=0;handles[i].writable=mode[0]=='w';
  return (void*)(uintptr_t)(i+1u);
 }
 assert(0);return 0;
}
int d88_file_native_close(void *p)
{unsigned h=handle(p);int fail=call(OP_CLOSE);handles[h].live=0;close_calls++;return fail?-1:0;}
unsigned d88_file_native_read(void *p,void *d,unsigned n)
{
 unsigned h=handle(p),f=handles[h].index;int fail=call(OP_READ);
 if(n>max_read) max_read=n;
 if(fail==1) return ~0u;
 if(n>files[f].size-handles[h].offset) n=files[f].size-handles[h].offset;
 assert(n<=CAPACITY && handles[h].offset+n<=CAPACITY);
 memcpy(d,files[f].data+handles[h].offset,n);handles[h].offset+=n;
 if(fail==2 && n) ((byte*)d)[0]^=1u;
 if(fail==3 && n) return n-1u;
 return n;
}
unsigned d88_file_native_write(void *p,const void *d,unsigned n)
{
 unsigned h=handle(p),f=handles[h].index;int fail=call(OP_WRITE);
 assert(handles[h].writable);
 if(fail==1) return ~0u;
 if(fail==2 && n) n--;
 assert(handles[h].offset+n<=CAPACITY);
 memcpy(files[f].data+handles[h].offset,d,n);handles[h].offset+=n;
 if(handles[h].offset>files[f].size) files[f].size=handles[h].offset;
 return n;
}
int d88_file_native_seek(void *p,unsigned o,unsigned whence)
{
 unsigned h=handle(p),f=handles[h].index;int fail=call(OP_SEEK);
 if(fail) return -1;
 if(whence==2u) o+=files[f].size;else if(whence==1u) o+=handles[h].offset;
 if(o>files[f].size) o=files[f].size;
 handles[h].offset=o;return (int)o;
}
int d88_file_native_tell(void *p) {unsigned h=handle(p);return call(OP_TELL)?-1:(int)handles[h].offset;}
int d88_file_native_error(void *p) {(void)handle(p);assert(in_gui && !lock_depth);return 0;}
int d88_file_native_exists(const char *p)
{int f;if(call(OP_EXISTS)) return -1;f=find(p);return f<0?0:(files[f].directory?-1:1);}
unsigned d88_file_native_errno(void) {return last_errno;}
int d88_file_native_remove(const char *p)
{
 int f=find(p),fail=call(OP_REMOVE);remove_calls++;
 if(cancel_remove) {cancel_remove=0;d88_file_cancel();}
 if(fail==1 || f<0) return -1;
 files[f].used=0;return fail==2?-1:0;
}
int d88_file_native_rename(const char *a,const char *b)
{
 int f=find(a),t=find(b),fail=call(OP_RENAME);rename_calls++;
 if(fail==1 || f<0 || t>=0) return -1;
 if(fail==3) {
  t=empty();assert(t>=0);files[t]=files[f];strcpy(files[t].name,b);return -1;
 }
 if(fail==4) {files[f].used=0;return -1;}
 strcpy(files[f].name,b);return fail==2?-1:0;
}
int d88_file_native_find_first(const char *p,unsigned a,d88_native_find *f)
{(void)p;(void)a;(void)f;assert(0);return -1;}
int d88_file_native_find_next(d88_native_find *f) {(void)f;assert(0);return -1;}
int d88_file_native_find_close(d88_native_find *f) {(void)f;assert(0);return -1;}
void model_reset(void)
{
 memset(files,0,sizeof(files));memset(handles,0,sizeof(handles));memset(counts,0,sizeof(counts));
 zero(&tx,sizeof(tx));zero(&reply,sizeof(reply));zero(request,sizeof(request));
 active=cancelled=0;fatal_error=cleanup_error=0;phase=P_START;next_job=next_token=1;
 profile_ok=1;in_gui=lock_depth=fs_calls=bad_calls=close_calls=rename_calls=remove_calls=0;
 fail_op=fail_nth=fail_mode=fail_seen=last_errno=cancel_remove=fail2_nth=fail2_mode=0;
 max_read=0;
 cancel_call_op=cancel_call_nth=cancel_call_seen=cancel_call_injections=0;
}
void model_step(void) {assert(!in_gui && !lock_depth);in_gui=1;d88_file_gui_step();in_gui=0;assert(!lock_depth);}
void model_seed(const char *p,const byte *data,unsigned n)
{int f=find(p);if(f<0) f=empty();assert(f>=0 && n<=CAPACITY);files[f].used=1;strcpy(files[f].name,p);memcpy(files[f].data,data,n);files[f].size=n;}
void model_directory(const char *p) {int f=empty();assert(f>=0);files[f].used=files[f].directory=1;strcpy(files[f].name,p);}
int model_size(const char *p) {int f=find(p);return f<0?-1:(int)files[f].size;}
unsigned model_contents(const char *p,byte *out) {int f=find(p);assert(f>=0);memcpy(out,files[f].data,files[f].size);return files[f].size;}
unsigned model_files(void) {unsigned i,n=0;for(i=0;i<FILES;i++) n+=files[i].used;return n;}
const char *model_name(unsigned i) {unsigned j,n=0;for(j=0;j<FILES;j++) if(files[j].used && n++==i) return files[j].name;return "";}
unsigned model_handles(void) {unsigned i,n=0;for(i=0;i<HANDLES;i++) n+=handles[i].live;return n;}
unsigned model_fs_calls(void) {return fs_calls;}
unsigned model_max_read(void) {return max_read;}
unsigned model_calls(unsigned op) {assert(op<16u);return counts[op];}
unsigned model_bad_calls(void) {return bad_calls;}
unsigned model_phase(void) {return phase;}
unsigned model_flags(void) {return tx.backup_owned|(tx.target_owned<<1)|(tx.temp_owned<<2)|(tx.verified<<3);}
void model_fail(unsigned op,unsigned nth,unsigned mode) {fail_op=op;fail_nth=nth;fail_mode=mode;fail_seen=0;}
void model_fail_also(unsigned nth,unsigned mode) {fail2_nth=nth;fail2_mode=mode;}
void model_profile(unsigned value) {profile_ok=value;}
void model_cancel_in_remove(void) {cancel_remove=1;}
void model_cancel_in_call(unsigned op,unsigned nth)
{assert(op>=OP_OPEN && op<=OP_TELL && nth);cancel_call_op=op;cancel_call_nth=nth;cancel_call_seen=0;}
unsigned model_cancel_injections(void) {return cancel_call_injections;}
void model_report_size(const char *p,unsigned n) {int f=find(p);assert(f>=0);files[f].size=n;}
#endif
