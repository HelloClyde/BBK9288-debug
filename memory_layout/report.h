#ifndef D88_LAYOUT_REPORT_H
#define D88_LAYOUT_REPORT_H
#include "memory_heap.h"
#include "static_arena.h"
typedef unsigned int layout_u32;
typedef struct d88_layout_page {
    unsigned address,bytes,crc32,zero_bytes,ff_bytes,first_word,last_word,reserved;
} d88_layout_page;
typedef struct d88_layout_record {
    unsigned address,size_flags,sample[4];
} d88_layout_record;
typedef struct d88_layout_report {
    unsigned header[64];
    unsigned iram[2][4096];
    d88_layout_page pages[128];
    d88_heap_snapshot heap_before,heap_after;
    d88_heap_allocation heap_work;
    d88_static_arena_report arena;
    unsigned tcbs[9][18];
    d88_layout_record records[256];
    unsigned crc32;
} d88_layout_report;
#define D88_LAYOUT_MAGIC 0x504d3844u
#define D88_LAYOUT_VERSION 1u
extern volatile d88_layout_report d88_layout_result;
#endif
