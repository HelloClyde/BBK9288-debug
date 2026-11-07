ENTRY(DL_AppMain)
SECTIONS
{
    __dp = 0;
    . = 0x02700000;
    .startup : { KEEP(*(.text.startup)) }
    ASSERT(SIZEOF(.startup) <= 0x100, "startup exceeds 0x100 bytes")
    .iram 0x00000800 : AT(0x02700100) { }
    .text 0x02701800 : AT(0x02701800) { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    . = ALIGN(4);
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    .payload_tail : { BYTE(0xff) }
    __payload_end = .;
    ASSERT(__payload_end <= 0x02800000, "application exceeds RAM window")
}
