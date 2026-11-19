/* A21s first stage: entered by Samsung S-Boot as the boot.img "kernel" (arm64 Image protocol):
 * MMU off, x0 = device tree. It copies itself out of the way (the Android kernel must later be
 * placed exactly where S-Boot put us) and continues at LINK_ADDR. */
#define LINK_ADDR 0xA8000000
    .section .text.boot, "ax"
    .global _start
_start:
    b       1f                      /* code0 */
    .long   0                       /* code1 */
    .quad   0x80000                 /* text_offset */
    .quad   __image_size            /* image_size */
    .quad   0xa                     /* flags: LE, 4K pages, anywhere */
    .long   0                       /* 0x20: original first instruction of the Android kernel (embedded mode) */
    .long   0                       /* 0x24: 0x44424d45 when the Android kernel precedes this image */
    .quad   0
    .quad   0
    .ascii  "ARM\x64"               /* magic */
    .long   0
1:
    mov     x19, x0                 /* dtb */
    mrs     x20, CurrentEL
    lsr     x20, x20, #2
    msr     daifset, #0xf
    adr     x1, _start              /* where we are */
    ldr     x2, =LINK_ADDR
    ldr     x3, =__copy_size
    add     x3, x3, #15
    and     x3, x3, #~15
    mov     x4, x1
    mov     x5, x2
2:  ldp     x6, x7, [x4], #16
    stp     x6, x7, [x5], #16
    subs    x3, x3, #16
    b.hi    2b
    dsb     sy
    ldr     x3, =__copy_size
    add     x3, x3, x2
    add     x3, x3, #64
    mov     x4, x2
8:  dc      cvau, x4
    add     x4, x4, #64
    cmp     x4, x3
    b.lo    8b
    dsb     ish
    ic      iallu
    dsb     sy
    isb
    ldr     x3, =reloc_entry
    br      x3
reloc_entry:
    ldr     x1, =__stack_top
    mov     sp, x1
    /* enable FP/SIMD in case the compiler uses it for struct copies */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.eq    3f
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    b       4f
3:  mrs     x1, cptr_el2
    bic     x1, x1, #(1 << 10)
    msr     cptr_el2, x1
4:  isb
    /* clear bss */
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
5:  cmp     x1, x2
    b.hs    6f
    str     xzr, [x1], #8
    b       5b
6:  mov     x0, x19
    mov     x1, x20
    bl      stage1_main
7:  wfe
    b       7b
