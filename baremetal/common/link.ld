/* Bare-metal test image for the QEMU m68k "virt" machine.
 * QEMU loads the ELF, jumps to the entry point with SR=0x2700 and
 * A7=0, and places the bootinfo list at (end of image + 1) & ~1. */
OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)

SECTIONS
{
    . = 0x00002000;
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : {
        . = ALIGN(1024);
        *(.vectors)
        *(.data .data.*)
    }
    . = ALIGN(16);
    .stacks (NOLOAD) : { *(.stacks) }
    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        _bss_end = .;
    }
    _end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
