/* Exercise actual read-only capture code with an independent MMIO whitelist. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define D88_MEM_MAP_TEST 1
#include "../memory/memory_map.c"

typedef struct model_register { unsigned address,width,value,reads; } model_register;
static model_register regs[]={
    {0x48122,2,0,0},{0x4812a,2,0x141,0},{0x4812e,2,0x8009,0},
    {0x48130,2,0xc00,0},{0x48132,2,0x1300,0},
    {0x4813c,1,0,0},{0x4813e,1,0,0},
    {0x300f30,1,0,0},{0x300f31,1,2,0},{0x300f32,1,0,0},
    {0x300f33,1,0x80,0},{0x300f34,1,0x80,0},{0x300f35,1,0,0},
    {0x300f36,1,0,0},{0x300f37,1,0,0},{0x300f38,1,1,0},{0x300f39,1,3,0},
    {0x40180,1,0x27,0},
    {0x300000,1,2,0},{0x300001,1,0x15,0},
    {0x300002,1,5,0},{0x300003,1,0,0},
    {0x3a0200,2,9,0},{0x3a0202,2,8,0},{0x3a0204,2,0x177,0},
    {0x3a0206,2,0x8f,0},{0x3a0208,2,2,0},{0x3a0210,2,0xf,0}
};
static unsigned initial_values[28],profile_reads,mmio_reads,bad_table,bad_signature;
static d88_mem_map_report capture;
static model_register *find(unsigned address)
{
    unsigned i;
    for(i=0;i<28u;i++) if(regs[i].address==address) return &regs[i];
    assert(!"MMIO read outside independent whitelist");return 0;
}
static unsigned value(unsigned address) {return find(address)->value;}
static void set(unsigned address,unsigned data) {find(address)->value=data;}
static unsigned mmio(unsigned address,unsigned width)
{
    model_register *reg=find(address);
    assert(reg->width==width);
    assert(profile_reads==126u); /* Six roots + all three 40-byte signatures. */
    if(address>=0x300000u) {
        assert((value(0x48132)&0x2222)==0x200 && (value(0x4812a)&0x700));
        assert(value(0x4812e)&1);
    }
    if(address>=0x3a0200u) {
        assert(value(0x300f33)&0x80);
        assert(!(value(0x300f35)&2) && !(value(0x300f36)&2));
        assert(!(value(0x300f37)&1));
        assert(value(0x300000)==2 && value(0x300001)==0x15 && value(0x300002)==5);
        assert(!(value(0x300003)&0xf0));
        if(address!=0x3a0200u) assert(value(0x3a0200)&8);
        if(address>=0x3a0204u) assert(value(0x3a0202)&8);
    }
    reg->reads++;mmio_reads++;return reg->value;
}
unsigned d88_mem_map_test_read32(unsigned address)
{
    unsigned index;
    assert(address>=0x02000200u && address<0x02000218u && !(address&3));
    index=(address-0x02000200u)/4u;profile_reads++;
    return profiles[1].tables[index]^((index==bad_table)?4u:0u);
}
unsigned short d88_mem_map_test_read16(unsigned address)
{return (unsigned short)mmio(address,2);}
unsigned char d88_mem_map_test_read8(unsigned address)
{
    unsigned i;
    if(address>=0x02000000u) {
        for(i=0;i<3u;i++) if(address>=profiles[1].signature_addresses[i] &&
           address<profiles[1].signature_addresses[i]+40u) {
            unsigned offset=address-profiles[1].signature_addresses[i];
            profile_reads++;
            return profiles[1].signatures[i][offset]^((address==bad_signature)?1u:0u);
        }
        assert(!"RAM read outside immutable profile whitelist");return 0;
    }
    return (unsigned char)mmio(address,1);
}
static void reset(void)
{
    unsigned i;
    for(i=0;i<28u;i++) {regs[i].value=initial_values[i];regs[i].reads=0;}
    profile_reads=mmio_reads=0;bad_table=bad_signature=~0u;
    memset(&capture,0xa5,sizeof(capture));
}
static void expect(int error,unsigned count)
{
    unsigned i,before[28];
    for(i=0;i<28u;i++) before[i]=regs[i].value;
    assert(d88_mem_map_capture(&capture)==error);
    assert(capture.version==1 && capture.structure_bytes==288u);
    assert(capture.result==(unsigned)error && capture.register_count==count);
    assert(capture.reserved==0);
    for(i=count;i<28u;i++) assert(!capture.registers[i].address && !capture.registers[i].value);
    for(i=0;i<28u;i++) assert(regs[i].value==before[i]); /* Zero model writes. */
    assert(mmio_reads==count);
    if(error) {
        assert(!capture.configured_bytes && !capture.bank_count && !capture.bank_bytes);
        assert(!capture.row_bytes && !capture.window_base && !capture.window_end);
        assert(!capture.organization && !(capture.flags&D88_MEM_MAP_F_RESOLVED));
    }
}
int main(void)
{
    unsigned i,n=0;
    assert(sizeof(capture)==288u && sizeof(capture.registers[0])==8u);
    for(i=0;i<28u;i++) initial_values[i]=regs[i].value;
    reset();assert(d88_mem_map_capture(0)==D88_MEM_MAP_E_ARGUMENT);
    assert(!profile_reads && !mmio_reads);n++;
    for(i=0;i<6u;i++) {reset();bad_table=i;expect(D88_MEM_MAP_E_PROFILE,0);n++;}
    for(i=0;i<3u;i++) {
        reset();bad_signature=profiles[1].signature_addresses[i]+39u;
        expect(D88_MEM_MAP_E_PROFILE,0);n++;
    }
    reset();expect(0,28);n++;
    assert(capture.flags==127u && capture.partrev==0x00051502u && capture.addrc==1);
    assert(capture.configured_bytes==0x800000 && capture.bank_count==4);
    assert(capture.bank_bytes==0x200000 && capture.row_bytes==512);
    assert(capture.area_enable_mask==3 && capture.window_base==0x2000000);
    assert(capture.window_end==0x2800000 && capture.organization==1);
    for(i=0;i<28u;i++) {
        assert(capture.registers[i].address==regs[i].address);
        assert(capture.registers[i].value==regs[i].value && regs[i].reads==1);
    }
    { static const unsigned a[]={0x48132,0x48132,0x48132,0x48132,0x4812a,0x4812e,0x48122,0x48130};
      static const unsigned v[]={0x1100,0x1302,0x3300,0x1320,0x41,0x8008,0x40,0x800};
      for(i=0;i<8u;i++) {reset();set(a[i],v[i]);expect(D88_MEM_MAP_E_BCU,7);n++;} }
    { static const unsigned a[]={0x300f33,0x300f35,0x300f36,0x300f37};
      static const unsigned v[]={0,2,2,1};
      for(i=0;i<4u;i++) {reset();set(a[i],v[i]);expect(D88_MEM_MAP_E_CLOCK,18);n++;} }
    for(i=0;i<4u;i++) {
        reset();set(0x300000+i,value(0x300000+i)^(i==3u?0x10u:1u));
        expect(D88_MEM_MAP_E_PART,22);n++;
    }
    reset();set(0x300003,0xf);expect(0,28);assert(capture.partrev==0x0f051502u);n++;
    reset();set(0x3a0200,1);expect(D88_MEM_MAP_E_SDRAM,23);n++;
    reset();set(0x3a0202,0);expect(D88_MEM_MAP_E_SDRAM,24);n++;
    for(i=0;i<8u;i++) if(i!=1u) {
        reset();set(0x3a0200,8u|i);expect(D88_MEM_MAP_E_ORGANIZATION,28);
        assert(capture.addrc==i && (capture.flags&D88_MEM_MAP_F_COMPLETE));n++;
    }
    reset();set(0x3a0210,1);expect(D88_MEM_MAP_E_ORGANIZATION,28);n++;
    reset();set(0x3a0210,3);expect(0,28);assert(capture.area_enable_mask==1);n++;
    reset();set(0x3a0210,5);expect(0,28);
    assert(capture.area_enable_mask==2 && capture.window_base==0x3000000u);
    assert(capture.window_end==0x3800000u);n++;
    reset();set(0x3a0204,0x321);set(0x3a0206,0x28f);set(0x3a0208,7);set(0x300f39,0);
    expect(0,28);assert(capture.registers[24].value==0x321);
    assert(capture.registers[25].value==0x28f && capture.registers[26].value==7);n++;
    printf("Memory map: PASS (%u cases; profile/gates, live geometry, strict read whitelist, zero MMIO writes)\n",n);
    return 0;
}
