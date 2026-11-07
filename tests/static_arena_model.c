/* Native execution of the production arena code with guarded private storage. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define D88_STATIC_ARENA_TEST 1
#include "../memory_layout/static_arena.c"

static unsigned memory[D88_STATIC_ARENA_BYTES/4u+2u];
static unsigned base,reads,writes,yields,fail_yield,corrupt_yield,corrupt_index;
unsigned d88_arena_test_base(void) {return base;}
static unsigned index_at(unsigned address)
{
    assert(address>=D88_STATIC_ARENA_BASE && address<D88_STATIC_ARENA_END && !(address&3u));
    return (address-D88_STATIC_ARENA_BASE)/4u+1u;
}
unsigned d88_arena_test_read32(unsigned address) {reads++;return memory[index_at(address)];}
void d88_arena_test_write32(unsigned address,unsigned value) {writes++;memory[index_at(address)]=value;}
static int yield(void)
{
    yields++;
    if(yields==corrupt_yield) memory[corrupt_index+1u]^=1u;
    return yields==fail_yield?-1:0;
}
static void setup(void)
{
    memset(memory,0x7b,sizeof(memory));memory[0]=0x12345678u;
    memory[sizeof(memory)/sizeof(memory[0])-1u]=0x87654321u;
    base=D88_STATIC_ARENA_BASE;reads=writes=yields=fail_yield=corrupt_yield=corrupt_index=0;
}
static void guards(void)
{
    assert(memory[0]==0x12345678u);
    assert(memory[sizeof(memory)/sizeof(memory[0])-1u]==0x87654321u);
}
int main(void)
{
    d88_static_arena_report out;
    const unsigned words=D88_STATIC_ARENA_BYTES/4u;
    setup();assert(!d88_static_arena_run(yield,&out));guards();
    assert(out.version==1u && out.structure_bytes==80u && out.flags==7u);
    assert(out.base==D88_STATIC_ARENA_BASE && out.arena_bytes==786432u && out.end==D88_STATIC_ARENA_END);
    assert(out.rounds_completed==2u && out.fill_words==2u*words && out.verified_words==2u*words);
    assert(out.yield_calls==192u && yields==192u && writes==2u*words && reads==2u*words);
    assert(out.expected_crc[0]==out.actual_crc[0] && out.expected_crc[1]==out.actual_crc[1]);
    assert(out.expected_crc[0]!=out.expected_crc[1] && !out.error_phase && !out.result);
    printf("crc round1=%08x round2=%08x\n",out.actual_crc[0],out.actual_crc[1]);
    setup();base+=16u;assert(d88_static_arena_run(yield,&out)==D88_STATIC_ARENA_E_LAYOUT);
    assert(!reads && !writes && !yields);guards();
    setup();assert(d88_static_arena_run(0,&out)==D88_STATIC_ARENA_E_ARGUMENT);
    assert(d88_static_arena_run(yield,0)==D88_STATIC_ARENA_E_ARGUMENT);
    assert(!reads && !writes && !yields);guards();
    setup();fail_yield=1;assert(d88_static_arena_run(yield,&out)==D88_STATIC_ARENA_E_YIELD);
    assert(writes==4096u && !reads && out.error_phase==1u && !out.rounds_completed);guards();
    setup();fail_yield=49;assert(d88_static_arena_run(yield,&out)==D88_STATIC_ARENA_E_YIELD);
    assert(writes==words && reads==4096u && out.error_phase==2u && !out.rounds_completed);guards();
    setup();corrupt_yield=48;corrupt_index=16385u;
    assert(d88_static_arena_run(yield,&out)==D88_STATIC_ARENA_E_VERIFY);
    assert(out.first_bad_address==D88_STATIC_ARENA_BASE+corrupt_index*4u);
    assert((out.first_bad_expected^out.first_bad_actual)==1u && out.rounds_completed==0u);guards();
    setup();corrupt_yield=144;corrupt_index=words-1u;
    assert(d88_static_arena_run(yield,&out)==D88_STATIC_ARENA_E_VERIFY);
    assert(out.first_bad_address==D88_STATIC_ARENA_END-4u && out.rounds_completed==1u && out.flags==3u);
    assert(out.expected_crc[0]==out.actual_crc[0]);guards();
    puts("static arena production model: PASS");return 0;
}
