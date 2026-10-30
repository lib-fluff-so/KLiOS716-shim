/* Current core transitions from identity-mapped EL1 to MMU-off execution.
 * Called with x0 = physical payload entry and x1 = physical boot_args pointer.
 * The current build keeps SCTLR.C/I clear, so no cache clean is required. */
    .section .text, "ax"
    .balign 16
    .globl transfer_to_payload
    .type transfer_to_payload, %function
transfer_to_payload:
    mov     x16, x0
    mov     x0, x1
    msr     daifset, #0xf
    dsb     sy
    tlbi    vmalle1
    dsb     sy
    isb
    mrs     x2, sctlr_el1
    bic     x2, x2, #1          /* SCTLR_EL1.M = 0 */
    bic     x2, x2, #4          /* SCTLR_EL1.C = 0 */
    bic     x2, x2, #0x1000     /* SCTLR_EL1.I = 0 */
    msr     sctlr_el1, x2
    isb
    ic      iallu
    dsb     sy
    isb
    br      x16
    .size transfer_to_payload, . - transfer_to_payload

/* Linux arm64 Image entry: x0 = physical DTB, x1-x3 = zero.
 * The shim was entered with MMU and caches disabled; keep that state. */
    .balign 16
    .globl transfer_to_linux
    .type transfer_to_linux, %function
transfer_to_linux:
    mov     x16, x0
    mov     x0, x1
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    msr     daifset, #0xf
    dsb     sy
    tlbi    vmalle1
    dsb     sy
    isb
    mrs     x4, sctlr_el1
    bic     x4, x4, #1
    bic     x4, x4, #4
    bic     x4, x4, #0x1000
    msr     sctlr_el1, x4
    isb
    ic      iallu
    dsb     sy
    isb
    br      x16
    .size transfer_to_linux, . - transfer_to_linux
