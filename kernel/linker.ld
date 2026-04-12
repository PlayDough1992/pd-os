/* PD-OS Kernel Linker Script
 * Maps kernel to 1MB physical address (0x100000)
 */

OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R-X */
    data PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS {
    . = 0x100000;

    _kernel_start = .;

    .text : { *(.text*) } :text

    .rodata : { *(.rodata*) } :text

    .data : { *(.data*) } :data

    .bss : {
        *(COMMON)
        *(.bss*)
    } :data

    _kernel_end = .;

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.comment)
        *(.eh_frame)
    }
}
