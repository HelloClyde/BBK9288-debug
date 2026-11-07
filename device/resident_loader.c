#include "resident_loader.h"

typedef __UINTPTR_TYPE__ resident_uintptr;

static resident_u16 read16(const resident_u8 *p)
{ return (resident_u16)((resident_u16)p[0] | ((resident_u16)p[1] << 8)); }
static resident_u32 read32(const resident_u8 *p)
{ return (resident_u32)read16(p) | ((resident_u32)read16(p + 2) << 16); }
static void write16(resident_u8 *p, resident_u16 v)
{ p[0] = (resident_u8)v; p[1] = (resident_u8)(v >> 8); }
static void write32(resident_u8 *p, resident_u32 v)
{ write16(p, (resident_u16)v); write16(p + 2, (resident_u16)(v >> 16)); }

static resident_u32 crc32(const resident_u8 *p, resident_u32 length)
{
    resident_u32 crc = 0xffffffffu, i, bit;
    for (i = 0; i < length; ++i) {
        crc ^= p[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return crc ^ 0xffffffffu;
}

static int power_of_two(resident_u32 n)
{ return n && !(n & (n - 1u)); }

static resident_u32 width(resident_u32 type)
{
    if (type == RESIDENT_RELOC_32) return 4;
    if (type >= RESIDENT_RELOC_ABS_H && type <= RESIDENT_RELOC_ABS_L) return 2;
    return 0;
}

static resident_u32 immediate(resident_u32 type, resident_u32 address)
{
    if (type == RESIDENT_RELOC_ABS_H) return (address >> 19) & 0x1fffu;
    if (type == RESIDENT_RELOC_ABS_M) return (address >> 6) & 0x1fffu;
    return (address & 0x3fu) << 4;
}

int resident_image_validate(const resident_blob *image)
{
    resident_u32 i, end = 0;
    if (!image || image->magic != RESIDENT_BLOB_MAGIC ||
        image->version != RESIDENT_BLOB_VERSION || !image->data ||
        !image->file_size || image->memory_size < image->file_size ||
        image->entry_offset >= image->file_size || (image->entry_offset & 1u) ||
        !power_of_two(image->alignment) || image->alignment < 2u ||
        image->alignment > 65536u || (image->linked_base & (image->alignment - 1u)) ||
        image->linked_base > 0xffffffffu - image->memory_size ||
        image->relocation_count > 0xffffffffu / sizeof(resident_relocation) ||
        (image->relocation_count && !image->relocations))
        return RESIDENT_LOAD_DESCRIPTOR;
    if (crc32(image->data, image->file_size) != image->data_crc32 ||
        crc32((const resident_u8 *)image->relocations,
              image->relocation_count * sizeof(resident_relocation)) != image->relocation_crc32)
        return RESIDENT_LOAD_CHECKSUM;
    for (i = 0; i < image->relocation_count; ++i) {
        const resident_relocation *r = image->relocations + i;
        resident_u32 bytes = width(r->type), address, encoded, mask;
        if (!bytes || r->offset > image->file_size ||
            bytes > image->file_size - r->offset || (r->offset & 1u) ||
            (i && r->offset < end) || r->target_offset > image->memory_size)
            return RESIDENT_LOAD_RELOCATION;
        end = r->offset + bytes;
        address = image->linked_base + r->target_offset;
        if (r->type == RESIDENT_RELOC_32) {
            if (read32(image->data + r->offset) != address)
                return RESIDENT_LOAD_RELOCATION;
        } else {
            encoded = read16(image->data + r->offset);
            mask = r->type == RESIDENT_RELOC_ABS_L ? 0x03f0u : 0x1fffu;
            if (r->type != RESIDENT_RELOC_ABS_L && (encoded & 0xe000u) != 0xc000u)
                return RESIDENT_LOAD_RELOCATION;
            if ((encoded & mask) != immediate(r->type, address))
                return RESIDENT_LOAD_RELOCATION;
        }
    }
    return RESIDENT_LOAD_OK;
}

static int overlap(const void *a, resident_u32 a_size, const void *b, resident_u32 b_size)
{
    resident_uintptr first = (resident_uintptr)a, second = (resident_uintptr)b;
    if (!a_size || !b_size) return 0;
    /* Subtract instead of forming a potentially overflowing end pointer. */
    return first <= second ? second - first < a_size : first - second < b_size;
}

int resident_image_load(const resident_blob *image, void *destination,
                        resident_u32 capacity, resident_u32 load_address,
                        resident_u32 *entry_address)
{
    resident_u32 i;
    resident_u8 *output = (resident_u8 *)destination;
    int result = resident_image_validate(image);
    if (result) return result;
    if (!destination || capacity < image->memory_size)
        return RESIDENT_LOAD_BUFFER;
    if ((load_address & (image->alignment - 1u)) ||
        load_address > 0xffffffffu - image->memory_size)
        return RESIDENT_LOAD_ADDRESS;
    if (overlap(destination, image->memory_size, image, sizeof(*image)) ||
        overlap(destination, image->memory_size, image->data, image->file_size) ||
        overlap(destination, image->memory_size, image->relocations,
                image->relocation_count * sizeof(resident_relocation)))
        return RESIDENT_LOAD_BUFFER;
    for (i = 0; i < image->file_size; ++i) output[i] = image->data[i];
    for (; i < image->memory_size; ++i) output[i] = 0;
    for (i = 0; i < image->relocation_count; ++i) {
        const resident_relocation *r = image->relocations + i;
        resident_u32 address = load_address + r->target_offset;
        if (r->type == RESIDENT_RELOC_32) write32(output + r->offset, address);
        else {
            resident_u32 mask = r->type == RESIDENT_RELOC_ABS_L ? 0x03f0u : 0x1fffu;
            resident_u32 encoded = read16(output + r->offset);
            write16(output + r->offset,
                    (resident_u16)((encoded & ~mask) | immediate(r->type, address)));
        }
    }
    if (entry_address) *entry_address = load_address + image->entry_offset;
    return RESIDENT_LOAD_OK;
}

int resident_image_allocate(const resident_blob *image,
                            void *(*allocate)(resident_u32),
                            void (*free_memory)(void *),
                            resident_loaded *loaded)
{
    resident_uintptr raw_address, aligned_address;
    resident_u32 reserve, entry;
    void *raw;
    int result = resident_image_validate(image);
    if (result) return result;
    if (!loaded || !allocate || !free_memory) return RESIDENT_LOAD_DESCRIPTOR;
    if (image->memory_size > 0xffffffffu - (image->alignment - 1u))
        return RESIDENT_LOAD_ADDRESS;
    reserve = image->memory_size + image->alignment - 1u;
    raw = allocate(reserve);
    if (!raw) return RESIDENT_LOAD_ALLOCATION;
    raw_address = (resident_uintptr)raw;
    if (raw_address > 0xffffffffu - reserve) {
        free_memory(raw);
        return RESIDENT_LOAD_ADDRESS;
    }
    aligned_address = (raw_address + image->alignment - 1u) &
                      ~(resident_uintptr)(image->alignment - 1u);
    result = resident_image_load(image, (void *)aligned_address,
                                 image->memory_size, (resident_u32)aligned_address, &entry);
    if (result) { free_memory(raw); return result; }
    loaded->allocation = raw;
    loaded->memory = (void *)aligned_address;
    loaded->size = image->memory_size;
    loaded->entry_address = entry;
    return RESIDENT_LOAD_OK;
}
