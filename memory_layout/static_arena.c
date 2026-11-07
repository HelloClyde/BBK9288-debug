#include "static_arena.h"

typedef char arena_word_is_32[(sizeof(unsigned)==4u)?1:-1];
typedef char arena_report_is_80[(sizeof(d88_static_arena_report)==80u)?1:-1];

#ifdef D88_STATIC_ARENA_TEST
extern unsigned d88_arena_test_base(void);
extern unsigned d88_arena_test_read32(unsigned address);
extern void d88_arena_test_write32(unsigned address,unsigned value);
#define BASE() d88_arena_test_base()
#define READ_WORD(i) d88_arena_test_read32(D88_STATIC_ARENA_BASE+(i)*4u)
#define WRITE_WORD(i,v) d88_arena_test_write32(D88_STATIC_ARENA_BASE+(i)*4u,(v))
#else
/* The linker owns the placement and NOLOAD property. A changed link script is
 * rejected at runtime before the first arena access. No global heap is reset. */
static volatile unsigned arena_words[D88_STATIC_ARENA_BYTES/4u]
    __attribute__((section(".app_arena"),aligned(16),used));
#define BASE() ((unsigned)arena_words)
#define READ_WORD(i) arena_words[i]
#define WRITE_WORD(i,v) (arena_words[i]=(v))
#endif

static unsigned pattern(unsigned index,unsigned round)
{
    unsigned value=(index^(index<<9)^(index>>7))+0xa55a3cc3u;
    return round?~value:value;
}
static unsigned crc_word(unsigned crc,unsigned value)
{
    unsigned byte,bit;
    for(byte=0;byte<4u;byte++) {
        crc^=value&255u;value>>=8;
        for(bit=0;bit<8u;bit++) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
    }
    return crc;
}
static int finish(d88_static_arena_report *out,int result)
{out->result=(unsigned)result;return result;}
static int cooperate(int (*yield)(void),d88_static_arena_report *out)
{out->yield_calls++;return yield()==0?0:D88_STATIC_ARENA_E_YIELD;}

int d88_static_arena_run(int (*yield)(void),d88_static_arena_report *out)
{
    unsigned i,round,expected,actual,crc;
    if(!out) return D88_STATIC_ARENA_E_ARGUMENT;
    for(i=0;i<sizeof(*out)/sizeof(unsigned);i++) ((unsigned*)out)[i]=0;
    out->version=D88_STATIC_ARENA_VERSION;out->structure_bytes=sizeof(*out);
    out->base=BASE();out->arena_bytes=D88_STATIC_ARENA_BYTES;
    out->end=out->base+D88_STATIC_ARENA_BYTES;
    if(!yield) return finish(out,D88_STATIC_ARENA_E_ARGUMENT);
    if(out->base!=D88_STATIC_ARENA_BASE || out->end!=D88_STATIC_ARENA_END)
        return finish(out,D88_STATIC_ARENA_E_LAYOUT);
    out->flags=D88_STATIC_ARENA_F_LAYOUT;
    for(round=0;round<2u;round++) {
        out->active_round=round+1u;out->error_phase=1u;crc=~0u;
        for(i=0;i<D88_STATIC_ARENA_BYTES/4u;i++) {
            expected=pattern(i,round);WRITE_WORD(i,expected);
            crc=crc_word(crc,expected);out->fill_words++;
            if((i&(D88_STATIC_ARENA_YIELD_BYTES/4u-1u))==D88_STATIC_ARENA_YIELD_BYTES/4u-1u)
                if(cooperate(yield,out)!=0) return finish(out,D88_STATIC_ARENA_E_YIELD);
        }
        out->expected_crc[round]=~crc;
        out->error_phase=2u;crc=~0u;
        for(i=0;i<D88_STATIC_ARENA_BYTES/4u;i++) {
            expected=pattern(i,round);actual=READ_WORD(i);
            crc=crc_word(crc,actual);out->verified_words++;
            if(actual!=expected) {
                out->first_bad_address=D88_STATIC_ARENA_BASE+i*4u;
                out->first_bad_expected=expected;out->first_bad_actual=actual;
                out->actual_crc[round]=~crc;
                return finish(out,D88_STATIC_ARENA_E_VERIFY);
            }
            if((i&(D88_STATIC_ARENA_YIELD_BYTES/4u-1u))==D88_STATIC_ARENA_YIELD_BYTES/4u-1u)
                if(cooperate(yield,out)!=0) return finish(out,D88_STATIC_ARENA_E_YIELD);
        }
        out->actual_crc[round]=~crc;
        if(out->actual_crc[round]!=out->expected_crc[round])
            return finish(out,D88_STATIC_ARENA_E_VERIFY);
        out->rounds_completed++;out->flags|=D88_STATIC_ARENA_F_ROUND1<<round;
    }
    out->error_phase=0;
    return finish(out,0);
}
