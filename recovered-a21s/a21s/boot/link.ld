ENTRY(_start)
SECTIONS
{
    . = 0xA8000000;
    _start = .;
    .text : { KEEP(*(.text.boot)) *(.text*) }
    .vectors ALIGN(2048) : { KEEP(*(.text.vec)) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    . = ALIGN(16);
    __copy_size = . - _start;
    __image_size = __copy_size;
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;
    . = ALIGN(0x1000);
    . += 0x40000;
    __stack_top = .;
    . += 0x10000;
    __exc_stack_top = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
