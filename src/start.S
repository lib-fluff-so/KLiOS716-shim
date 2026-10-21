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

/* PSCI CPU_ON entry for secondary cores: MMU off, x0 = &struct sec_ctx (context_id).
 * ctx: +0 stack_top +8 mair +16 tcr +24 ttbr0 +32 state +40 mpidr +48 sctlr +56 el
 * state 1 = entered, 2 = MMU on with the same tables as the boot CPU.
 *
 * Besides the shared-memory report, every core paints a 32x32 square on the
 * framebuffer (column = Aff1, so cpu1..7 -> 8 slots): orange as the very first
 * thing it does, red + park if its context reads as zeros, blue just before the
 * MMU enable, green once the MMU is on, then keeps repainting. That proof does
 * not depend on cache coherence with the boot CPU. FB row 900 of the visible area,
 * stride 4352. */
    .equ FB_SQ, 0xFDFFF000 + (1230 + 900) * 4352
    .equ FB_STRIDE, 4352

    .macro PAINT color
        mrs     x5, mpidr_el1
        ubfx    x5, x5, #8, #8
        ldr     x7, =FB_SQ
        mov     x8, #288
        madd    x7, x5, x8, x7
        add     x7, x7, #160
        ldr     w6, =\color
        mov     w9, #32
        mov     x12, #FB_STRIDE
    1:  mov     x10, x7
        mov     w11, #32
    2:  str     w6, [x10], #4
        subs    w11, w11, #1
        b.ne    2b
        add     x7, x7, x12
        subs    w9, w9, #1
        b.ne    1b
    .endm

    .text
    .balign 4
    .globl secondary_entry
    .type secondary_entry, %function
secondary_entry:
    msr     daifset, #0xf
    mov     x4, x0                  /* PAINT clobbers x5-x12 only */
    PAINT   0xFFFFA500              /* orange: entered */
    mov     x0, x4
    msr     spsel, #1
    ldr     x1, [x0, #0]
    cbz     x1, .Lstale             /* context looks like zeros: boot CPU's writes not visible */
    ldr     x2, [x0, #16]
    cbz     x2, .Lstale
    mov     sp, x1
    adr     x9, exception_vectors
    msr     vbar_el1, x9
    isb
    mrs     x2, mpidr_el1
    str     x2, [x0, #40]
    mrs     x2, currentel
    str     x2, [x0, #56]
    mov     x2, #1
    str     x2, [x0, #32]
    dsb     sy
    sev
    PAINT   0xFF0000FF              /* blue: context valid, enabling MMU now */
    ldr     x2, [x0, #8]
    msr     mair_el1, x2
    ldr     x2, [x0, #16]
    msr     tcr_el1, x2
    ldr     x2, [x0, #24]
    msr     ttbr0_el1, x2
    isb
    tlbi    vmalle1
    dsb     nsh
    isb
    ic      iallu
    dsb     nsh
    isb
    mrs     x2, sctlr_el1
    orr     x2, x2, #1
    bic     x2, x2, #2
    bic     x2, x2, #4
    bic     x2, x2, #0x1000
    msr     sctlr_el1, x2
    isb
    mrs     x3, sctlr_el1
    str     x3, [x0, #48]
    mov     x2, #2
    str     x2, [x0, #32]
    dsb     sy
    sev
.Lstale:
    PAINT   0xFFFF0000              /* red: stale/zero context, parked */
    b       .Lstale
.Lrepaint:
    PAINT   0xFF00FF00              /* green: MMU on, keep repainting */
    b       .Lrepaint
    .ltorg
    .size secondary_entry, . - secondary_entry

    .section .stack, "aw", %progbits
    .balign 16
    .space  0x10000
    .globl stack_top
stack_top:
