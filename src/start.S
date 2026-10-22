/* Entry for LK: arm64 Image header + position-independent startup.
 * LK jumps here with MMU off, x0 = DTB pointer (arm64 boot protocol). */
    .section .text.head, "ax"
    .globl _start
_start:
    b       1f                  /* code0 */
    .word   0                   /* code1 */
    .quad   0                   /* text_offset */
    .quad   __image_size        /* image_size */
    .quad   0x8                 /* flags: LE, 4K unspecified, placement anywhere */
    .quad   0, 0, 0             /* res2..res4 */
    .ascii  "ARM\x64"           /* magic */
    .word   0                   /* res5 */
1:
    adr     x9, stack_top
    /* Select SP_EL1; writing SP_EL1 directly is UNDEFINED at EL1. */
    msr     spsel, #1
    mov     sp, x9
    isb

    /* Install the 2 KiB-aligned exception table before entering C. */
    adr     x9, exception_vectors
    msr     vbar_el1, x9
    isb

    adr     x4, _start
    bl      shim_main
2:
    wfe
    b       2b

    .section .stack, "aw", %progbits
    .balign 16
    .space  0x10000
    .globl stack_top
stack_top:
