/* Minimal EL1 exception vectors: preserve only the exception state needed for
 * a compact panic record. Every vector is 128 bytes; VBAR_EL1 is 2 KiB aligned. */
    .section .text.vectors, "ax"
    .balign 2048
    .globl exception_vectors
    .type exception_vectors, %function
exception_vectors:
    .macro VECTOR_SLOT
        b       exception_common
        .space  124
    .endm
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    VECTOR_SLOT
    .size exception_vectors, . - exception_vectors

    .type exception_common, %function
exception_common:
    msr     daifset, #0xf
    mrs     x0, esr_el1
    mrs     x1, far_el1
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    b       exception_c_entry
    .size exception_common, . - exception_common
