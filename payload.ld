ENTRY(test_payload_entry)
SECTIONS
{
    . = 0x140000000;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) *(.rodata .rodata.*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.got*) *(.interp) }
}
