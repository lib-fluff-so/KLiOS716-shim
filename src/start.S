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
 * ctx (128 bytes): +0 stack_top +8 mair +16 tcr +24 ttbr0 +32 state +40 mpidr +48 sctlr +56 el +64 x0_seen
 * state 1 = entered, 2 = MMU on with the same tables as the boot CPU.
 * Diagnostic results are stored in shared context memory; no diagnostic UI is
 * painted by secondary CPUs, so the screen remains limited to the bottom bar. */
    .text
    .balign 4
    .globl secondary_entry
    .type secondary_entry, %function
/* Does NOT trust x0/context_id (MTK's TF-A may not pass it): the core finds its own
 * context from __seccpu_start + Aff1*128 and computes stack, MAIR, TCR and TTBR0 itself.
 * It stores the x0 it was given at ctx+64 so the boot CPU can log it. */
secondary_entry:
    msr     daifset, #0xf
    mov     x4, x0                  /* preserve incoming context_id */

    mrs     x2, mpidr_el1
    ubfx    x5, x2, #8, #8          /* idx = Aff1 */
    adrp    x1, __seccpu_start
    add     x1, x1, :lo12:__seccpu_start
    lsl     x6, x5, #7
    add     x0, x1, x6              /* x0 = ctx = __seccpu_start + idx*128 */
    str     x4, [x0, #64]           /* x0 as received */
    str     x2, [x0, #40]           /* mpidr */
    mrs     x3, currentel
    str     x3, [x0, #56]

    add     x6, x5, #1              /* stack top = seccpu + 0x1000 + (idx+1)*0x2000 */
    lsl     x6, x6, #13
    add     x6, x6, x1
    add     x6, x6, #0x1000
    msr     spsel, #1
    mov     sp, x6
    adr     x9, exception_vectors
    msr     vbar_el1, x9
    isb

    mov     x3, #1
    str     x3, [x0, #32]           /* state = 1 */
    dsb     sy
    sev

    /* MAIR: Attr0 Device-nGnRnE, Attr1 Normal NC, Attr2 Normal WB */
    mov     x2, #0x4400
    movk    x2, #0xff, lsl #16
    msr     mair_el1, x2
    /* TCR: T0SZ=24, walks NC/OSH, TG0=16K, EPD1, TG1=16K, IPS=min(PARange,5) */
    mov     x2, #0xa018
    movk    x2, #0x4080, lsl #16
    mrs     x3, id_aa64mmfr0_el1
    and     x3, x3, #0xf
    mov     x7, #5
    cmp     x3, x7
    csel    x3, x3, x7, ls
    orr     x2, x2, x3, lsl #32
    msr     tcr_el1, x2
    /* TTBR0 = first 16 KiB-aligned table of the pool (the level-1 table) */
    adrp    x2, __pgtbl_start
    add     x2, x2, :lo12:__pgtbl_start
    mov     x3, #0x3fff
    add     x2, x2, x3
    bic     x2, x2, x3
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
    mov     x3, #2
    str     x3, [x0, #32]           /* state = 2 */
    dsb     sy
    sev
.Lpark:
    wfe
    b       .Lpark
    .ltorg
    .size secondary_entry, . - secondary_entry

    .section .stack, "aw", %progbits
    .balign 16
    .space  0x10000
    .globl stack_top
stack_top:
