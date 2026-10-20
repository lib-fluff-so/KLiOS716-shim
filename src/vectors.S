/*
 * Minimal AArch64 EL1 exception vectors for ios-kl7.
 *
 * Each architectural vector slot is exactly 128 bytes. The full table is
 * aligned to 2 KiB as required by VBAR_EL1. Every slot lands in the same
 * best-effort diagnostic path; this does not attempt exception recovery.
 */
    .section .text.vectors, "ax"
    .balign 2048
    .globl exception_vectors
    .type exception_vectors, %function
exception_vectors:
    .macro VECTOR_SLOT
        b       exception_common
        .space  124
    .endm

    /* Current EL using SP0: sync, IRQ, FIQ, SError. */
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    /* Current EL using SPx: sync, IRQ, FIQ, SError. */
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    /* Lower EL using AArch64: sync, IRQ, FIQ, SError. */
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    /* Lower EL using AArch32: sync, IRQ, FIQ, SError. */
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    .size exception_vectors, . - exception_vectors

    .type exception_common, %function
exception_common:
    /* Stop asynchronous re-entry before building the diagnostic frame. */
    msr     daifset, #0xf
    /* Save x0-x30 plus the exception state. 304 bytes preserves 16-byte SP
     * alignment before calling C. Current EL1 code is forced onto SP_EL1. */
    sub     sp, sp, #304
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    stp     x30, xzr, [sp, #240]

    mrs     x16, esr_el1
    str     x16, [sp, #256]
    mrs     x16, far_el1
    str     x16, [sp, #264]
    mrs     x16, elr_el1
    str     x16, [sp, #272]
    mrs     x16, spsr_el1
    str     x16, [sp, #280]
    add     x16, sp, #304
    str     x16, [sp, #288]
    mrs     x16, vbar_el1
    str     x16, [sp, #296]

    mov     x0, sp
    bl      exception_c_entry
1:
    wfe
    b       1b
    .size exception_common, . - exception_common
