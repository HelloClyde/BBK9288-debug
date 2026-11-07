ENTRY(resident_entry)
SECTIONS
{
    __dp = 0;
    . = DEFINED(RESIDENT_BASE) ? RESIDENT_BASE : 0x00100000;
    __resident_start = .;
    .text : { *(.text.resident_entry) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    . = ALIGN(16);
    __resident_file_end = .;
    .bss (NOLOAD) : {
        __resident_bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(16);
        __resident_bss_end = .;
    }
    __resident_end = .;
    ASSERT(__resident_start % 16 == 0, "resident base must be aligned")
    ASSERT(__resident_end - __resident_start <= 0x001f0000, "resident exceeds heap budget")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
