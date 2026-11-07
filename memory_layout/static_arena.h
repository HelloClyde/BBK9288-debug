#ifndef D88_STATIC_ARENA_H
#define D88_STATIC_ARENA_H

#define D88_STATIC_ARENA_VERSION 1u
#define D88_STATIC_ARENA_BASE 0x02740000u
#define D88_STATIC_ARENA_BYTES (768u*1024u)
#define D88_STATIC_ARENA_END 0x02800000u
#define D88_STATIC_ARENA_YIELD_BYTES 16384u
#define D88_STATIC_ARENA_F_LAYOUT 1u
#define D88_STATIC_ARENA_F_ROUND1 2u
#define D88_STATIC_ARENA_F_ROUND2 4u

typedef struct d88_static_arena_report {
    unsigned version,structure_bytes,result,flags;
    unsigned base,arena_bytes,end,rounds_completed;
    unsigned fill_words,verified_words,yield_calls,active_round;
    unsigned expected_crc[2],actual_crc[2];
    unsigned first_bad_address,first_bad_expected,first_bad_actual,error_phase;
} d88_static_arena_report;

enum {
    D88_STATIC_ARENA_E_ARGUMENT=-901,
    D88_STATIC_ARENA_E_LAYOUT=-902,
    D88_STATIC_ARENA_E_YIELD=-903,
    D88_STATIC_ARENA_E_VERIFY=-904
};

/* Link .app_arena as a NOLOAD section at BASE..END, outside .bss/payload.
 * This function initializes every word explicitly and never relies on loader
 * clearing. It touches only its own arena and the caller's separate report.
 * No heap/clock/IRAM/USB access occurs. The caller retains native heap ownership
 * separately and can prove simultaneous use by checking its block afterwards.
 * yield() must be an ordinary foreground-task callback returning 0 on success;
 * it runs after each 16KiB chunk, outside all locks, including verification.
 * Do not start another download application while this arena is in use. Its
 * contents have only foreground application lifetime and are not resident. */
int d88_static_arena_run(int (*yield)(void),d88_static_arena_report *out);

#endif
