ENTRY(_start)
SECTIONS
{
    . = 0;
    .text   : { KEEP(*(.text.head)) KEEP(*(.text.vectors)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) *(.bss .bss.* COMMON) }
    . = ALIGN(16);
    .stack  : { *(.stack) _end = .; }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.got*) *(.interp) }
}
__image_size = _end;
