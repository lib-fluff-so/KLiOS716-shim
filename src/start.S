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
    mov     x19, x0
    mov     x20, x1
    mov     x21, x2
    mov     x22, x3

    /* Set the banked EL1 stack explicitly, regardless of the incoming SPSel. */
    adr     x9, stack_top
    msr     sp_el1, x9
    msr     spsel, #1
    isb

    /* Install the 2 KiB-aligned vector table before entering C. */
    adr     x9, exception_vectors
    msr     vbar_el1, x9
    isb

    mov     x0, x19
    mov     x1, x20
    mov     x2, x21
    mov     x3, x22
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
