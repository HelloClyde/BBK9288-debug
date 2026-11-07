#ifndef BBK9288_DEBUG_CORE_H
#define BBK9288_DEBUG_CORE_H

/* Independent of an OS, allocator, USB controller, and C runtime. */
typedef unsigned int dbg_u32;
typedef unsigned short dbg_u16;
typedef unsigned char dbg_u8;
typedef char dbg_u32_must_be_32_bits[(sizeof(dbg_u32) == 4) ? 1 : -1];
typedef char dbg_u16_must_be_16_bits[(sizeof(dbg_u16) == 2) ? 1 : -1];

#define DBG_MAX_PAYLOAD 512u
#define DBG_FRAME_BYTES 528u
#define DBG_SCRATCH_BYTES 512u
#define DBG_IDENTITY "BBK9288DEBUG/1 CAPTURED-0215EBC4 EXPERIMENTAL"

#define DBG_CAP_READ     (1u << 0)
#define DBG_CAP_WRITE    (1u << 1)
#define DBG_CAP_KEY      (1u << 2)
#define DBG_CAP_SCREEN   (1u << 3)
#define DBG_CAP_STOP     (1u << 4)
#define DBG_CAP_SCRATCH  (1u << 5)
#define DBG_CAP_FILES    (1u << 6)
#define DBG_CAP_APP_REFRESH (1u << 7)
#define DBG_CAP_ALL      0xffu
#define DBG_FLAG_FOREGROUND (1u << 0)
#define DBG_FLAG_HARDWARE_UNVERIFIED (1u << 1)
#define DBG_FLAG_UPTIME_UNAVAILABLE (1u << 2)
#define DBG_FLAG_GUI_KEYS_ONLY (1u << 3)

#define DBG_OK 0u
#define DBG_INVALID 1u
#define DBG_BUSY 2u
#define DBG_IO 3u
#define DBG_NEED_PING 4u

typedef struct dbg_screen {
    dbg_u32 width, height, bpp, stride, format, flags;
} dbg_screen;

typedef struct dbg_platform {
    void *context;
    dbg_u32 image_id, caps, flags, scratch_address;
    dbg_u8 *snapshot;
    dbg_u32 snapshot_capacity;
    dbg_u32 (*uptime_ms)(void *context);
    /* read returns 0 only after exactly length bytes were copied. */
    int (*read)(void *context, dbg_u32 address, dbg_u8 *output, dbg_u32 length);
    /* key accepts one complete DOWN+UP job; 0 means accepted, not drawn. */
    int (*key)(void *context, dbg_u32 scan);
    /* capture returns byte count >0, or a negative error, and fills metadata. */
    int (*capture)(void *context, dbg_u8 *output, dbg_u32 capacity, dbg_screen *screen);
    /* File requests are asynchronous; callback itself may only queue/poll.
     * Native filesystem work runs separately in its verified GUI task. */
    int (*file_rpc)(void *context,dbg_u32 opcode,const dbg_u8 *input,dbg_u32 length,
                    dbg_u8 *output,dbg_u32 *output_length);
    void (*file_reset)(void *context);
} dbg_platform;

typedef struct dbg_state {
    dbg_platform platform;
    dbg_u8 rx[DBG_FRAME_BYTES], tx[DBG_FRAME_BYTES];
    dbg_u8 scratch[DBG_SCRATCH_BYTES];
    dbg_u32 rx_used, rx_expected, discard_remaining, tx_length;
    dbg_u32 image_id, caps, requests, errors, token, next_token;
    dbg_u32 snapshot_bytes, session, ready, stop_pending, stop_requested;
    dbg_screen screen;
} dbg_state;

void dbg_init(dbg_state *state, const dbg_platform *platform);
/* Feed returns the number consumed. Stop feeding while a reply is outstanding;
 * retain any remaining input in the transport until that reply is consumed. */
unsigned dbg_feed(dbg_state *state, const dbg_u8 *input, unsigned length);
const dbg_u8 *dbg_reply(dbg_state *state, unsigned *length);
/* Call only after every reply byte (including a required transport ZLP) left
 * the device. STOP becomes stop_requested here, never during dispatch. */
void dbg_consume_reply(dbg_state *state);
/* USB reset/disconnect starts a new unauthorized session and invalidates the
 * snapshot token, partial input, reply, and deferred STOP. Statistics remain. */
void dbg_reset_session(dbg_state *state);

#endif
