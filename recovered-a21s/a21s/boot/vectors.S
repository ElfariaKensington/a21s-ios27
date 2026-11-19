/* EL1 exception vectors for the iOS entry: any exception is logged to eMMC and the phone restarts (back into Android). */
    .section .text.vec, "ax"
    .balign 2048
    .global vectors
vectors:
    .rept 16
    .balign 128
    b       exc_common
    .endr
exc_common:
    ldr     x0, =__exc_stack_top
    mov     sp, x0
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    mov     x3, x30
    bl      exc_handler
1:  wfe
    b       1b
