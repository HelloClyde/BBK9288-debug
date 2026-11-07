#ifndef D88_MEMORY_HEAP_H
#define D88_MEMORY_HEAP_H

#define D88_MEMORY_HEAP_VERSION 1u
#define D88_MEMORY_HEAP_MAX_RECORDS 256u
#define D88_MEMORY_HEAP_GUI_RESERVE (128u*1024u)

/* All report fields are u32; signed errors use their two's-complement bits. */
typedef struct d88_heap_snapshot {
    unsigned version,bytes,result;
    unsigned start,end,data_top,directory_bottom,record_count;
    unsigned allocated_records,free_records,allocated_payload,free_holes;
    unsigned largest_hole,tail_gap,tail_capacity,max_request,free_capacity;
    unsigned metadata_bytes,directory_crc32;
} d88_heap_snapshot;

typedef struct d88_heap_probe {
    unsigned version,bytes,result,candidate,pointer;
    unsigned boundary_request,boundary_pointer;
    unsigned allocated_ok,boundary_failed,state_restored,directory_restored;
    unsigned errno_before,errno_after;
    d88_heap_snapshot before,after;
} d88_heap_probe;

typedef struct d88_heap_allocation {
    unsigned version,bytes,result,address,allocation_bytes,active;
} d88_heap_allocation;

enum {
    D88_MEMORY_HEAP_E_ARGUMENT=-501,
    D88_MEMORY_HEAP_E_CONTEXT=-502,
    D88_MEMORY_HEAP_E_PROFILE=-503,
    D88_MEMORY_HEAP_E_LOCK=-504,
    D88_MEMORY_HEAP_E_UNLOCK=-505,
    D88_MEMORY_HEAP_E_LAYOUT=-506,
    D88_MEMORY_HEAP_E_LIMIT=-507,
    D88_MEMORY_HEAP_E_ALLOC=-508,
    D88_MEMORY_HEAP_E_NATIVE=-509,
    D88_MEMORY_HEAP_E_RESTORE=-510,
    D88_MEMORY_HEAP_E_BOUNDARY=-511,
    D88_MEMORY_HEAP_E_RESERVE=-512,
    D88_MEMORY_HEAP_E_BUSY=-513,
    D88_MEMORY_HEAP_E_OWNER=-514,
    D88_MEMORY_HEAP_E_FAULT=-515
};

/* Ordinary native task context only, with interrupts enabled and no inherited
 * CPU/dispatch lock. Fingerprints and metadata are rechecked under loc_cpu.
 * No function initializes the heap or reads/writes allocation payload bytes. */
int d88_memory_heap_snapshot(d88_heap_snapshot *out);
/* Exact inferred maximum, immediate free, then max+1 failure boundary, all
 * inside one bounded CPU lock. Native errno and active metadata are restored.
 * Integrity failure latches a fault and prevents further allocator calls. */
int d88_memory_heap_probe(d88_heap_probe *out);
/* One owned benchmark allocation. Keep at least128KiB total AND contiguous
 * native GUI headroom. Allocation/free use short locks; benchmark outside.
 * The report address is usable only on return0. */
int d88_memory_heap_allocate(unsigned requested,d88_heap_allocation *out);
int d88_memory_heap_release(void);
/* Overflow-safe check within this module's current owned payload. */
int d88_memory_heap_owns(unsigned address,unsigned bytes);
unsigned d88_memory_heap_fault(void);

#endif
