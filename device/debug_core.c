#include "debug_core.h"

static const dbg_u8 magic[4] = {'8', '8', 'D', 'B'};

static void zero_bytes(void *pointer, dbg_u32 length)
{
    dbg_u8 *output = (dbg_u8 *)pointer;
    dbg_u32 i;
    for (i = 0; i < length; ++i) output[i] = 0;
}

static void copy_bytes(dbg_u8 *output, const dbg_u8 *input, dbg_u32 length)
{
    dbg_u32 i;
    for (i = 0; i < length; ++i) output[i] = input[i];
}

static dbg_u32 get16(const dbg_u8 *input)
{
    return (dbg_u32)input[0] | ((dbg_u32)input[1] << 8);
}

static dbg_u32 get32(const dbg_u8 *input)
{
    return get16(input) | (get16(input + 2) << 16);
}

static void put16(dbg_u8 *output, dbg_u32 value)
{
    output[0] = (dbg_u8)value;
    output[1] = (dbg_u8)(value >> 8);
}

static void put32(dbg_u8 *output, dbg_u32 value)
{
    put16(output, value);
    put16(output + 2, value >> 16);
}

static dbg_u32 crc32(const dbg_u8 *input, dbg_u32 length)
{
    dbg_u32 value = 0xffffffffu, i, bit;
    for (i = 0; i < length; ++i) {
        value ^= input[i];
        for (bit = 0; bit < 8u; ++bit)
            value = (value >> 1) ^ ((0u - (value & 1u)) & 0xedb88320u);
    }
    return value ^ 0xffffffffu;
}

static dbg_u32 callback_error(int result)
{
    if (result < 0 && result >= -(int)DBG_NEED_PING) return (dbg_u32)(-result);
    return DBG_IO;
}

static int contains(dbg_u32 base, dbg_u32 capacity, dbg_u32 address, dbg_u32 length)
{
    return length && address >= base && address - base <= capacity &&
           length <= capacity - (address - base);
}

static int scratch_valid(const dbg_state *s)
{
    return s->platform.scratch_address &&
           s->platform.scratch_address <= 0xffffffffu - DBG_SCRATCH_BYTES + 1u;
}

static int screen_valid(const dbg_state *s, dbg_u32 size)
{
    const dbg_screen *m = &s->screen;
    dbg_u32 minimum;
    if (!m->width || m->width > 1024u || !m->height || m->height > 1024u)
        return 0;
    if (!((m->format == 1u && m->bpp == 8u) || (m->format == 2u && m->bpp == 2u)))
        return 0;
    minimum = (m->width * m->bpp + 7u) / 8u;
    return m->stride >= minimum && m->stride <= minimum + 64u &&
           size == m->stride * m->height && size <= s->platform.snapshot_capacity &&
           size <= 1024u * 1024u;
}

static dbg_u32 dispatch(dbg_state *s, dbg_u32 op, const dbg_u8 *body,
                       dbg_u32 length, dbg_u8 *output, dbg_u32 *status)
{
    dbg_u32 address, count, offset, i, file_length;
    int result;
    if (op == 1u) {
        if (length) { *status = DBG_INVALID; return 0; }
        s->session = 1;
        copy_bytes(output, (const dbg_u8 *)DBG_IDENTITY, sizeof(DBG_IDENTITY) - 1u);
        return sizeof(DBG_IDENTITY) - 1u;
    }
    if (!s->session) { *status = DBG_NEED_PING; return 0; }
    switch (op) {
    case 2u:
        if (length) break;
        put32(output, s->image_id);
        put32(output + 4, s->caps);
        put32(output + 8, s->requests);
        put32(output + 12, s->errors);
        put32(output + 16, s->platform.uptime_ms ? s->platform.uptime_ms(s->platform.context) : 0u);
        put32(output + 20, s->token);
        put32(output + 24, s->snapshot_bytes);
        put32(output + 28, s->platform.flags | DBG_FLAG_HARDWARE_UNVERIFIED);
        return 32u;
    case 3u:
        if (length != 8u || !(s->caps & DBG_CAP_READ)) break;
        address = get32(body); count = get32(body + 4);
        if (!count || count > DBG_MAX_PAYLOAD || address > 0xffffffffu - count + 1u) break;
        if (scratch_valid(s) && contains(s->platform.scratch_address, DBG_SCRATCH_BYTES, address, count)) {
            copy_bytes(output, s->scratch + (address - s->platform.scratch_address), count);
            return count;
        }
        if (!s->platform.read) break;
        result = s->platform.read(s->platform.context, address, output, count);
        if (result) { *status = callback_error(result); return 0; }
        return count;
    case 4u:
        if (length != 4u || !(s->caps & DBG_CAP_KEY)) break;
        count = get32(body);
        if (!count || count > 255u) break;
        result = s->platform.key(s->platform.context, count);
        if (result) *status = callback_error(result);
        return 0;
    case 5u:
        if (length || !(s->caps & DBG_CAP_SCREEN)) break;
        if (s->token) { *status = DBG_BUSY; return 0; }
        zero_bytes(&s->screen, sizeof(s->screen));
        result = s->platform.capture(s->platform.context, s->platform.snapshot,
                                     s->platform.snapshot_capacity, &s->screen);
        if (result <= 0) { *status = callback_error(result); return 0; }
        if (!screen_valid(s, (dbg_u32)result)) { *status = DBG_IO; return 0; }
        ++s->next_token;
        if (!s->next_token) ++s->next_token;
        s->token = s->next_token;
        s->snapshot_bytes = (dbg_u32)result;
        put32(output, s->token);
        put32(output + 4, s->screen.width);
        put32(output + 8, s->screen.height);
        put32(output + 12, s->screen.bpp);
        put32(output + 16, s->screen.stride);
        put32(output + 20, s->snapshot_bytes);
        put32(output + 24, s->screen.format);
        put32(output + 28, s->screen.flags);
        return 32u;
    case 6u:
        if (length != 12u || !(s->caps & DBG_CAP_SCREEN)) break;
        offset = get32(body + 4); count = get32(body + 8);
        if (!s->token || get32(body) != s->token || !count || count > DBG_MAX_PAYLOAD ||
            offset > s->snapshot_bytes || count > s->snapshot_bytes - offset) break;
        copy_bytes(output, s->platform.snapshot + offset, count);
        return count;
    case 7u:
        if (length != 4u || !(s->caps & DBG_CAP_SCREEN) || !s->token || get32(body) != s->token) break;
        s->token = 0; s->snapshot_bytes = 0;
        return 0;
    case 8u:
        if (length || !(s->caps & DBG_CAP_STOP)) break;
        s->stop_pending = 1;
        return 0;
    case 9u:
        if (length < 5u || !(s->caps & DBG_CAP_WRITE)) break;
        address = get32(body); count = length - 4u;
        if (!contains(s->platform.scratch_address, DBG_SCRATCH_BYTES, address, count)) break;
        offset = address - s->platform.scratch_address;
        for (i = 0; i < count; ++i) s->scratch[offset + i] = body[4u + i];
        return 0;
    case 10u:
        if (length || !(s->caps & DBG_CAP_SCRATCH)) break;
        put32(output, s->platform.scratch_address);
        put32(output + 4, DBG_SCRATCH_BYTES);
        return 8u;
    default:
        if(((op>=11u && op<=20u && (s->caps&DBG_CAP_FILES)) ||
            (op==21u && (s->caps&DBG_CAP_APP_REFRESH))) && s->platform.file_rpc) {
            file_length=0;
            result=s->platform.file_rpc(s->platform.context,op,body,length,output,&file_length);
            if(result) {*status=callback_error(result);return 0;}
            if(file_length>DBG_MAX_PAYLOAD) {*status=DBG_IO;return 0;}
            return file_length;
        }
        break;
    }
    *status = DBG_INVALID;
    return 0;
}

static void complete_frame(dbg_state *s)
{
    dbg_u32 payload = get16(s->rx + 8), size, status = DBG_OK;
    dbg_u32 op = s->rx[5];
    if (crc32(s->rx, 12u + payload) != get32(s->rx + 12u + payload)) {
        ++s->errors;
        return;
    }
    if (s->rx[4] != 1u || !op || op > 127u) {
        ++s->errors;
        return;
    }
    ++s->requests;
    if (get16(s->rx + 10)) {
        status = DBG_INVALID;
        size = 0;
    } else {
        size = dispatch(s, op, s->rx + 12, payload, s->tx + 12, &status);
    }
    if (status) ++s->errors;
    copy_bytes(s->tx, magic, 4u);
    s->tx[4] = 1u; s->tx[5] = (dbg_u8)(op | 128u);
    put16(s->tx + 6, get16(s->rx + 6));
    put16(s->tx + 8, size); put16(s->tx + 10, status);
    put32(s->tx + 12u + size, crc32(s->tx, 12u + size));
    s->tx_length = size + 16u;
    s->ready = 1;
}

void dbg_reset_session(dbg_state *s)
{
    if(s->platform.file_reset) s->platform.file_reset(s->platform.context);
    s->rx_used = 0; s->rx_expected = 0; s->discard_remaining = 0;
    s->tx_length = 0; s->ready = 0; s->session = 0;
    s->token = 0; s->snapshot_bytes = 0;
    s->stop_pending = 0; s->stop_requested = 0;
}

void dbg_init(dbg_state *s, const dbg_platform *platform)
{
    zero_bytes(s, sizeof(*s));
    if (!platform) return;
    copy_bytes((dbg_u8 *)&s->platform, (const dbg_u8 *)platform, sizeof(*platform));
    s->image_id = platform->image_id;
    s->caps = platform->caps & DBG_CAP_ALL;
    if (!scratch_valid(s)) s->caps &= ~(DBG_CAP_WRITE | DBG_CAP_SCRATCH);
    if (!platform->read && !scratch_valid(s)) s->caps &= ~DBG_CAP_READ;
    if (!platform->key) s->caps &= ~DBG_CAP_KEY;
    if (!platform->capture || !platform->snapshot || !platform->snapshot_capacity)
        s->caps &= ~DBG_CAP_SCREEN;
    if (!platform->file_rpc || !platform->file_reset) s->caps &= ~(DBG_CAP_FILES|DBG_CAP_APP_REFRESH);
    if (!(s->caps&DBG_CAP_FILES)) s->caps &= ~DBG_CAP_APP_REFRESH;
}

unsigned dbg_feed(dbg_state *s, const dbg_u8 *input, unsigned length)
{
    unsigned used = 0;
    dbg_u8 byte;
    while (used < length && !s->ready && !s->stop_requested) {
        byte = input[used++];
        if (s->discard_remaining) { --s->discard_remaining; continue; }
        if (s->rx_used < 4u) {
            if (byte == magic[s->rx_used]) {
                s->rx[s->rx_used++] = byte;
            } else {
                /* Keep the longest possible prefix, including overlapping 88. */
                if (byte == magic[0]) {
                    s->rx[0] = byte;
                    s->rx_used = (s->rx_used == 2u) ? 2u : 1u;
                    if (s->rx_used == 2u) s->rx[1] = byte;
                } else s->rx_used = 0;
            }
            continue;
        }
        s->rx[s->rx_used++] = byte;
        if (s->rx_used == 12u) {
            dbg_u32 payload = get16(s->rx + 8);
            if (payload > DBG_MAX_PAYLOAD) {
                ++s->errors;
                s->discard_remaining = payload + 4u;
                s->rx_used = 0; s->rx_expected = 0;
                continue;
            }
            s->rx_expected = payload + 16u;
        }
        if (s->rx_expected && s->rx_used == s->rx_expected) {
            complete_frame(s);
            s->rx_used = 0; s->rx_expected = 0;
        }
    }
    return used;
}

const dbg_u8 *dbg_reply(dbg_state *s, unsigned *length)
{
    if (length) *length = s->ready ? s->tx_length : 0u;
    return s->ready ? s->tx : (const dbg_u8 *)0;
}

void dbg_consume_reply(dbg_state *s)
{
    if (!s->ready) return;
    s->ready = 0; s->tx_length = 0;
    if (s->stop_pending) { s->stop_pending = 0; s->stop_requested = 1; }
}
