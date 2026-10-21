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
/* Zero-initialised, not part of the file: translation tables, per-CPU contexts and stacks.
 * Placed right after the image in RAM; the runtime checks it against the DTB map. */
SECTIONS
{
    .pgtbl (NOLOAD) : ALIGN(0x4000) {
        __pgtbl_start = .;
        . += 0x200000;               /* 128 x 16 KiB translation tables */
        __pgtbl_end = .;
        . = ALIGN(16);
        __seccpu_start = .;
        . += 0x11000;                /* 4 KiB contexts + 8 x 8 KiB secondary stacks */
        __seccpu_end = .;
    }
}
__image_size = _end;
