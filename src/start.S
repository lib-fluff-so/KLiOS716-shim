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

    adr     x9, stack_top
#ifndef EMULATOR_COMPAT
    /* Select SP_EL1 and set it through MOV SP. An explicit MSR SP_EL1 is
     * UNDEFINED at EL1 on normal ARMv8-A implementations. */
    msr     spsel, #1
#endif
    mov     sp, x9
    isb

#ifndef EMULATOR_COMPAT
    /* Install the 2 KiB-aligned vector table before entering C. The Unicorn
     * harness is instruction-level, not an EL1 system emulator, so it skips
     * privileged system-register setup in its separate compatibility build. */
    adr     x9, exception_vectors
    msr     vbar_el1, x9
    isb
#endif

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
