/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K
    RAM (rw)   : ORIGIN = 0x20000000, LENGTH = 16K
}

__flash_size = LENGTH(FLASH);
__ram_size = LENGTH(RAM);
__stack_size = 2K;
__stack_top = ABSOLUTE(ORIGIN(RAM) + LENGTH(RAM));
__stack_limit = __stack_top - __stack_size;

SECTIONS
{
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab*)
    } > FLASH

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    .init_array :
    {
        KEEP(*(.preinit_array* .init_array* .fini_array*))
    } > FLASH

    .data :
    {
        . = ALIGN(4);
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM

    /DISCARD/ :
    {
        *(.comment .note.GNU-stack)
    }
}

ASSERT(SIZEOF(.init_array) == 0, "Use constexpr/constinit globals; runtime constructors are unsupported")
ASSERT(__bss_end <= __stack_limit, "Static RAM overlaps the reserved 2 KiB stack")
