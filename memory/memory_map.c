/* Fixed read-only inventory for captured 0215EBC4 firmware. The only memory
 * writes are to the caller's report; MMIO addresses/widths are explicit. */
#include "memory_map.h"

typedef unsigned u32;
typedef unsigned char u8;
typedef unsigned short u16;
#include "../device/profiles.h"

typedef char map_word_is_32[(sizeof(u32)==4u)?1:-1];
typedef char map_halfword_is_16[(sizeof(u16)==2u)?1:-1];
typedef char map_report_is_fixed[(sizeof(d88_mem_map_report)==D88_MEM_MAP_REPORT_BYTES)?1:-1];

#ifdef D88_MEM_MAP_TEST
extern unsigned d88_mem_map_test_read32(unsigned address);
extern unsigned short d88_mem_map_test_read16(unsigned address);
extern unsigned char d88_mem_map_test_read8(unsigned address);
#define R32(a) d88_mem_map_test_read32(a)
#define R16(a) d88_mem_map_test_read16(a)
#define R8(a) d88_mem_map_test_read8(a)
#else
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R8(a) (*(volatile u8 *)(a))
#endif

static int profile_valid(void)
{
    const probe_profile *profile=&profiles[1];
    unsigned i,b;
    for(i=0;i<6u;i++)
        if(R32(0x02000200u+4u*i)!=profile->tables[i]) return 0;
    for(i=0;i<3u;i++) for(b=0;b<40u;b++)
        if(R8(profile->signature_addresses[i]+b)!=profile->signatures[i][b]) return 0;
    return 1;
}
static unsigned record(d88_mem_map_report *report,unsigned address,unsigned value)
{
    unsigned index=report->register_count++;
    report->registers[index].address=address;
    report->registers[index].value=value;
    return value;
}
static unsigned read8(d88_mem_map_report *report,unsigned address)
{ return record(report,address,R8(address)); }
static unsigned read16(d88_mem_map_report *report,unsigned address)
{ return record(report,address,R16(address)); }
static int result(d88_mem_map_report *report,int error)
{ report->result=(unsigned)error; return error; }

int d88_mem_map_capture(d88_mem_map_report *report)
{
    unsigned i,area14,area6,bus,ce,mapping,clock[10],part,sdon,initialized,app;
    unsigned char *bytes=(unsigned char *)report;
    if(!report) return D88_MEM_MAP_E_ARGUMENT;
    for(i=0;i<sizeof(*report);i++) bytes[i]=0;
    report->version=D88_MEM_MAP_VERSION;
    report->structure_bytes=sizeof(*report);
    if(!profile_valid()) return result(report,D88_MEM_MAP_E_PROFILE);
    report->flags|=D88_MEM_MAP_F_PROFILE;

    /* Manual II-4: internal/little-endian area6 with >=1 wait enables the
     * peripheral window. Area13/14 must be external little-endian x16, with
     * extended CE selection, before interpreting its SDRAMC mapping. */
    area14=read16(report,0x00048122u);
    area6=read16(report,0x0004812au);
    bus=read16(report,0x0004812eu);
    ce=read16(report,0x00048130u);
    mapping=read16(report,0x00048132u);
    (void)read8(report,0x0004813cu);
    (void)read8(report,0x0004813eu);
    if((mapping&0x2222u)!=0x0200u || !(area6&0x0700u) ||
       !(bus&1u) || (area14&0x40u) || !((ce>>9)&3u))
        return result(report,D88_MEM_MAP_E_BCU);
    report->flags|=D88_MEM_MAP_F_BCU;

    /* III-17: bus clock generator must run, subsystem source/SDRAM/arbiter
     * clocks must be supplied. Observe existing settings without waking it. */
    for(i=0;i<10u;i++) clock[i]=read8(report,0x00300f30u+i);
    (void)read8(report,0x00040180u);
    if(!(clock[3]&0x80u) || (clock[5]&2u) ||
       (clock[6]&2u) || (clock[7]&1u))
        return result(report,D88_MEM_MAP_E_CLOCK);
    report->flags|=D88_MEM_MAP_F_CLOCK;

    /* III-2-4: immutable CID/MID/NAME and VER registers, all byte reads.
     * VER is its upper nibble; the lower nibble is reserved. No USB access. */
    part=0;
    for(i=0;i<4u;i++) part|=read8(report,0x00300000u+i)<<(8u*i);
    report->partrev=part;
    if((part&0x00ffffffu)!=0x00051502u || (part&0xf0000000u))
        return result(report,D88_MEM_MAP_E_PART);
    report->flags|=D88_MEM_MAP_F_PART;

    sdon=read16(report,0x003a0200u);
    report->addrc=sdon&7u;
    if(!(sdon&8u)) return result(report,D88_MEM_MAP_E_SDRAM);
    initialized=read16(report,0x003a0202u);
    if(!(initialized&8u)) return result(report,D88_MEM_MAP_E_SDRAM);
    report->flags|=D88_MEM_MAP_F_SDRAM;
    (void)read16(report,0x003a0204u);
    (void)read16(report,0x003a0206u);
    (void)read16(report,0x003a0208u);
    app=read16(report,0x003a0210u);
    report->area_enable_mask=(app>>1)&3u;
    report->flags|=D88_MEM_MAP_F_COMPLETE;
    /* Only the captured organization is resolved. Raw values remain useful
     * if live ADDRC/area state differs; never substitute emulator defaults. */
    if(report->addrc!=1u || !report->area_enable_mask)
        return result(report,D88_MEM_MAP_E_ORGANIZATION);
    report->configured_bytes=8u*1024u*1024u;
    report->bank_count=4u;
    report->bank_bytes=2u*1024u*1024u;
    report->row_bytes=512u;
    report->window_base=(report->area_enable_mask&1u)?0x02000000u:0x03000000u;
    report->window_end=report->window_base+report->configured_bytes;
    report->organization=D88_MEM_MAP_ORGANIZATION_4M_X16;
    report->flags|=D88_MEM_MAP_F_RESOLVED;
    return result(report,D88_MEM_MAP_OK);
}
