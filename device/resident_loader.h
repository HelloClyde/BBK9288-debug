#ifndef D88_RESIDENT_LOADER_H
#define D88_RESIDENT_LOADER_H

/* This loader is independent of ROS33 and the download application's tables.
 * The caller owns the allocation and must retain it while any task, callback,
 * saved return address or queued event can reference the resident image. */
typedef unsigned char resident_u8;
typedef unsigned short resident_u16;
typedef unsigned int resident_u32;
typedef char resident_u32_is_32_bits[(sizeof(resident_u32) == 4) ? 1 : -1];

#define RESIDENT_BLOB_MAGIC 0x52383844u /* D88R */
#define RESIDENT_BLOB_VERSION 1u
#define RESIDENT_RELOC_32 2u
#define RESIDENT_RELOC_ABS_H 3u
#define RESIDENT_RELOC_ABS_M 4u
#define RESIDENT_RELOC_ABS_L 5u

typedef struct resident_relocation {
    resident_u32 offset;
    resident_u32 target_offset;
    resident_u32 type;
} resident_relocation;
typedef char resident_relocation_is_12_bytes[(sizeof(resident_relocation) == 12) ? 1 : -1];

typedef struct resident_blob {
    resident_u32 magic, version;
    const resident_u8 *data;
    resident_u32 file_size, memory_size, alignment, entry_offset, linked_base;
    const resident_relocation *relocations;
    resident_u32 relocation_count, data_crc32, relocation_crc32;
} resident_blob;

typedef struct resident_loaded {
    void *allocation; /* raw allocator result; do not free while resident is live */
    void *memory;     /* aligned self_base for relocation and runtime control */
    resident_u32 size, entry_address;
} resident_loaded;

enum resident_load_error {
    RESIDENT_LOAD_OK = 0,
    RESIDENT_LOAD_DESCRIPTOR = -1,
    RESIDENT_LOAD_CHECKSUM = -2,
    RESIDENT_LOAD_RELOCATION = -3,
    RESIDENT_LOAD_BUFFER = -4,
    RESIDENT_LOAD_ALLOCATION = -5,
    RESIDENT_LOAD_ADDRESS = -6
};

/* Validate the complete descriptor and linked encoding without writing. */
int resident_image_validate(const resident_blob *image);

/* destination has capacity bytes; load_address is the 32-bit address at which
 * those bytes will execute. Explicit load_address also permits native tests to
 * validate the production loader without mapping RAM at device addresses.
 * No destination bytes are written until all validation has passed. */
int resident_image_load(const resident_blob *image, void *destination,
                        resident_u32 capacity, resident_u32 load_address,
                        resident_u32 *entry_address);

/* Convenience for the 32-bit target. The allocated block is aligned according
 * to image->alignment. On failure before entry is called, free_memory is used
 * to roll back. On success the caller takes ownership of BOTH returned
 * addresses and must not discard a live image. This does not call entry,
 * create a task, install hooks, or change hardware. */
int resident_image_allocate(const resident_blob *image,
                            void *(*allocate)(resident_u32),
                            void (*free_memory)(void *),
                            resident_loaded *loaded);

#endif
