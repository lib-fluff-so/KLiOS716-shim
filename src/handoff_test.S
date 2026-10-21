/* Tiny position-independent test payload. The loader wraps this byte stream in
 * a CRC32 header, copies it to 0x140000000 and enters it with MMU/cache off,
 * x0 = struct test_boot_args *. It writes proof fields then branches back to
 * the shim's physical return trampoline. No stack, libc, or relocations used. */
    .section .text.entry, "ax"
    .balign 16
    .globl test_payload_entry
    .type test_payload_entry, %function
test_payload_entry:
    mov     x19, x0
    adr     x1, test_payload_entry
    str     x1, [x19, #80]         /* payload_pc */
    str     x0, [x19, #104]        /* x0_seen */
    mrs     x1, sctlr_el1
    str     x1, [x19, #88]         /* payload_sctlr */
    mrs     x1, CurrentEL
    str     x1, [x19, #96]         /* payload_currentel */
    movz    x1, #0x4c4b
    movk    x1, #0x5437, lsl #16
    movk    x1, #0x4553, lsl #32
    movk    x1, #0x3054, lsl #48 /* "KL7TEST0" in little-endian memory */
    str     x1, [x19, #72]         /* payload_magic */
    ldr     x16, [x19, #64]        /* return_address */
    dsb     sy
    br      x16
    .size test_payload_entry, . - test_payload_entry
