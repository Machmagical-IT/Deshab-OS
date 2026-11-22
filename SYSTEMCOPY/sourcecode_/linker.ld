/* Nutro+OS kernel linker script - flat 32-bit protected mode kernel @ 1 MiB */
ENTRY(_start)

SECTIONS
{
    . = 0x00100000;
    __kernel_start = .;

    .text : {
        *(.text.start)
        *(.text*)
    }

    .rodata ALIGN(16) : {
        *(.rodata*)
    }

    .data ALIGN(16) : {
        *(.data*)
    }

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss*)
        *(COMMON)
    }
    . = ALIGN(16);
    __bss_end = .;
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
    }
}