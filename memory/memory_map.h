#ifndef D88_MEMORY_MAP_H
#define D88_MEMORY_MAP_H

#define D88_MEM_MAP_VERSION 1u
#define D88_MEM_MAP_REGISTERS 28u
#define D88_MEM_MAP_REPORT_BYTES 288u

typedef struct d88_mem_map_register { unsigned address,value; } d88_mem_map_register;
typedef struct d88_mem_map_report {
    unsigned version,structure_bytes,result,flags;
    unsigned register_count,partrev,addrc,configured_bytes;
    unsigned bank_count,bank_bytes,row_bytes,area_enable_mask;
    unsigned window_base,window_end,organization,reserved;
    d88_mem_map_register registers[D88_MEM_MAP_REGISTERS];
} d88_mem_map_report;

enum {
    D88_MEM_MAP_F_PROFILE=1u,
    D88_MEM_MAP_F_BCU=2u,
    D88_MEM_MAP_F_CLOCK=4u,
    D88_MEM_MAP_F_PART=8u,
    D88_MEM_MAP_F_SDRAM=16u,
    D88_MEM_MAP_F_COMPLETE=32u,
    D88_MEM_MAP_F_RESOLVED=64u
};
enum {
    D88_MEM_MAP_OK=0,
    D88_MEM_MAP_E_ARGUMENT=-1,
    D88_MEM_MAP_E_PROFILE=-2,
    D88_MEM_MAP_E_BCU=-3,
    D88_MEM_MAP_E_CLOCK=-4,
    D88_MEM_MAP_E_PART=-5,
    D88_MEM_MAP_E_SDRAM=-6,
    D88_MEM_MAP_E_ORGANIZATION=-7
};
/* Organization is controller configuration, never physical package count. */
#define D88_MEM_MAP_ORGANIZATION_4M_X16 1u

/* Fixed 288-byte u32 layout. Result contains the return value's u32 bits;
 * window_end is exclusive, area_enable_mask bit0=area13 / bit1=area14.
 * Partrev packs CID/MID/NAME/VER bytes at 00300000..3, low byte first.
 * Rejected captures retain only fields/registers already read safely.
 * No syscall, USB access, register write, or unowned scratch write occurs.
 * Only the caller's report is initialized/written. An accepted profile plus
 * BCU/clock/part/SDON/SDEN gates is required before timing-register reads. */
int d88_mem_map_capture(d88_mem_map_report *report);

#endif
