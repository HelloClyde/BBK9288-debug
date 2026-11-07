/* Independent foreground probe. IRAM/system SDRAM are read only. Writes are
 * confined to this linked download image/arena and one guarded malloc block. */
#include "dsys.h"
#include "power_idle.h"
#include "report.h"
typedef unsigned int u32;
typedef unsigned char u8;
#include "profiles.h"
#ifndef LAYOUT_IMAGE_ID
#define LAYOUT_IMAGE_ID 0u
#endif
volatile d88_layout_report d88_layout_result;
static unsigned crc_table[256];
static d88_heap_snapshot before,after;
static d88_heap_allocation work;
static d88_static_arena_report arena_report;
extern unsigned char __bss_start,__bss_end,__payload_end,__app_arena_end;
static unsigned psr(void) {unsigned n;__asm__ volatile("ld.w %0, %%psr":"=r"(n)::"memory");return n;}
static unsigned sp(void) {unsigned n;__asm__ volatile("ld.w %0, %%sp":"=r"(n)::"memory");return n;}
static unsigned gp(void) {unsigned n;__asm__ volatile("ld.w %0, %%r15":"=r"(n)::"memory");return n;}
static unsigned r32(unsigned a) {return *(volatile unsigned*)a;}
static void zero(void *p,unsigned n) {unsigned char *b=p;while(n--) *b++=0;}
static void copy_out(volatile void *p,const void *s,unsigned n)
{volatile unsigned char *d=p;const unsigned char *b=s;while(n--) *d++=*b++;}
static void table_init(void)
{unsigned n,i,c;for(n=0;n<256u;n++){c=n;for(i=0;i<8u;i++)c=(c>>1)^((0u-(c&1u))&0xedb88320u);crc_table[n]=c;}}
static unsigned crc_byte(unsigned c,unsigned b) {return (c>>8)^crc_table[(c^b)&255u];}
static unsigned crc_word(unsigned c,unsigned v)
{c=crc_byte(c,v);c=crc_byte(c,v>>8);c=crc_byte(c,v>>16);return crc_byte(c,v>>24);}
static int profile(void)
{unsigned i,j;const probe_profile *p=profiles+1;
 for(i=0;i<6u;i++)if(r32(0x02000200u+4u*i)!=p->tables[i])return 0;
 for(i=0;i<3u;i++)for(j=0;j<40u;j++)if(*(volatile u8*)(p->signature_addresses[i]+j)!=p->signatures[i][j])return 0;
 return gp()==0u && (psr()&0x710u)==0x10u;
}
static int yield_cpu(void)
{int r=d88_power_idle_poll();return r?r:tpDL_ROS33Table->ros33_dly_tsk(1);}
static void section(unsigned kind,const volatile void *p,unsigned bytes,unsigned count)
{unsigned n=d88_layout_result.header[18],at=24u+n*4u;
 if(n>=9u)return;
 d88_layout_result.header[at]=kind;
 d88_layout_result.header[at+1u]=(unsigned)p-(unsigned)&d88_layout_result;
 d88_layout_result.header[at+2u]=bytes;d88_layout_result.header[at+3u]=count;
 d88_layout_result.header[18]=n+1u;
}
static void seal(unsigned state)
{unsigned i,c=~0u,b;const volatile unsigned char *p=(const volatile unsigned char*)&d88_layout_result;
 /* Publish state last. Compute its final bytes virtually so a reader cannot
  * observe DONE before the entire standard CRC32 trailer is committed. */
 for(i=0;i<sizeof(d88_layout_result)-4u;i++) {
  b=(i>=16u && i<20u)?((state>>(8u*(i-16u)))&255u):p[i];c=crc_byte(c,b);
 }
 d88_layout_result.crc32=~c;__asm__ volatile("":::"memory");d88_layout_result.header[4]=state;
}
static int capture_iram(void)
{unsigned i,k;for(k=0;k<2u;k++) {
 for(i=0;i<4096u;i++)d88_layout_result.iram[k][i]=r32(i*4u);
 d88_layout_result.header[14u+k]=r32(0x02068674u);
 if(yield_cpu())return -1002;
 }return 0;}
static int capture_pages(void)
{unsigned page,i,v,c,z,f,b,a;volatile d88_layout_page *p;
 for(page=0;page<128u;page++) {
  a=0x02000000u+page*65536u;c=~0u;z=f=0;
  for(i=0;i<16384u;i++) {
   v=r32(a+i*4u);c=crc_word(c,v);
   for(b=0;b<4u;b++){unsigned x=(v>>(8u*b))&255u;z+=x==0u;f+=x==255u;}
  }
  p=&d88_layout_result.pages[page];p->address=a;p->bytes=65536u;p->crc32=~c;
  p->zero_bytes=z;p->ff_bytes=f;p->first_word=r32(a);p->last_word=r32(a+65532u);
  d88_layout_result.header[7]=page+1u;if(yield_cpu())return -1002;
 }return 0;}
static int capture_tables(void)
{unsigned t=r32(0x022a9820u),i,j,p,s;
 if(t!=0x02068b34u || before.record_count>256u)return -1003;
 for(i=0;i<9u;i++)for(j=0;j<18u;j++)d88_layout_result.tcbs[i][j]=r32(t+i*72u+j*4u);
 for(i=0;i<before.record_count;i++) {
  p=r32(before.end-(i+1u)*8u);s=r32(before.end-(i+1u)*8u+4u);
  if((p&3u) || p<before.start || p>=before.end || (s&~1u)>before.end-p)return -1003;
  d88_layout_result.records[i].address=p;d88_layout_result.records[i].size_flags=s;
  if((s&1u) && (s&~1u)>=16u)for(j=0;j<4u;j++)d88_layout_result.records[i].sample[j]=r32(p+j*4u);
 }return 0;}
static unsigned heap_pattern(unsigned i) {return 0x6c38a5d2u^(i*0x01010101u);}
static int fill_heap(void)
{unsigned i;volatile unsigned *p=(volatile unsigned*)work.address;
 if(!d88_memory_heap_owns(work.address,work.allocation_bytes))return -1004;
 for(i=0;i<work.allocation_bytes/4u;i++) {p[i]=heap_pattern(i);if((i&4095u)==4095u && yield_cpu())return -1002;}
 return 0;}
static int verify_heap(void)
{unsigned i;volatile unsigned *p=(volatile unsigned*)work.address;
 if(!d88_memory_heap_owns(work.address,work.allocation_bytes))return -1004;
 for(i=0;i<work.allocation_bytes/4u;i++) {if(p[i]!=heap_pattern(i))return -1005;if((i&4095u)==4095u && yield_cpu())return -1002;}
 return 0;}
static int equal_snapshot(const d88_heap_snapshot *a,const d88_heap_snapshot *b)
{unsigned i;const unsigned *x=(const unsigned*)a,*y=(const unsigned*)b;for(i=0;i<sizeof(*a)/4u;i++)if(x[i]!=y[i])return 0;return 1;}
static int run(void)
{int result=0,cleanup;volatile unsigned *h=d88_layout_result.header;
 zero((void*)&d88_layout_result,sizeof(d88_layout_result));table_init();
 h[0]=D88_LAYOUT_MAGIC;h[1]=1u;h[2]=sizeof(d88_layout_result);h[3]=LAYOUT_IMAGE_ID;h[4]=2u;
 h[9]=psr();h[10]=sp();h[11]=gp();h[12]=r32(0x02068664u);h[13]=r32(0x02068674u);
 if(!profile()){result=-1001;goto done;}h[8]|=1u;
 result=d88_power_idle_begin();if(result)goto done;
 h[5]=1u;result=capture_iram();if(result)goto done;h[8]|=2u;
 result=d88_memory_heap_snapshot(&before);if(result)goto done;
 result=capture_tables();if(result)goto done;
 h[5]=2u;result=capture_pages();if(result)goto done;h[8]|=4u;
 h[5]=3u;h[20]=1572864u;result=d88_memory_heap_allocate(h[20],&work);
 if(result) {h[20]=1048576u;result=d88_memory_heap_allocate(h[20],&work);}
 if(result)goto done;h[8]|=8u;h[21]=work.allocation_bytes;
 result=fill_heap();if(result)goto done;
 h[5]=4u;result=d88_static_arena_run(yield_cpu,&arena_report);if(result)goto done;
 h[22]=arena_report.arena_bytes;
 result=verify_heap();if(result)goto done;
 h[19]=work.allocation_bytes+arena_report.arena_bytes;h[8]|=16u;
done:
 h[17]=h[5];h[5]=5u;cleanup=d88_memory_heap_release();
 if(!cleanup){h[8]|=32u;work.active=0;}else if(!result)result=cleanup;
 cleanup=d88_memory_heap_snapshot(&after);if(cleanup && !result)result=cleanup;
 if(!result && !equal_snapshot(&before,&after))result=-1006;
 d88_power_idle_end();h[6]=(unsigned)result;h[16]=r32(0x02068674u);
 copy_out(&d88_layout_result.heap_before,&before,sizeof(before));copy_out(&d88_layout_result.heap_after,&after,sizeof(after));
 copy_out(&d88_layout_result.heap_work,&work,sizeof(work));copy_out(&d88_layout_result.arena,&arena_report,sizeof(arena_report));
 section(1u,d88_layout_result.iram[0],16384u,1u);section(2u,d88_layout_result.iram[1],16384u,1u);
 section(3u,d88_layout_result.pages,sizeof(d88_layout_result.pages),128u);
 section(4u,&d88_layout_result.heap_before,sizeof(before),1u);section(5u,&d88_layout_result.heap_after,sizeof(after),1u);
 section(6u,&d88_layout_result.heap_work,sizeof(work),1u);section(7u,&d88_layout_result.arena,sizeof(arena_report),1u);
 section(8u,d88_layout_result.tcbs,sizeof(d88_layout_result.tcbs),9u);
 section(9u,d88_layout_result.records,sizeof(d88_layout_result.records),before.record_count);
 h[5]=0x100u;seal(result?4u:3u);return result;
}
T_WORD App_Main(void)
{int result;
 if(!profile())return -1;
 tpDL_GUITable->MessageBox(0,(const T_BYTE*)"Read-only system/IRAM census.\nTest only own program arena and malloc block.\nPress OK to begin.",(const T_BYTE*)"D88MAP START",MB_OK);
 result=run();
 tpDL_GUITable->MessageBox(0,(const T_BYTE*)(result?"Probe stopped. Leave result open for PC.":"Layout and private memory test complete.\nLeave result open for PC."),(const T_BYTE*)(result?"D88MAP ERROR":"D88MAP DONE"),MB_OK);
 return result;
}
