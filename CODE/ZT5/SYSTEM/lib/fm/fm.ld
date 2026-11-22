/* User program linker script for file manager */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

SECTIONS
{
    . = 0x0000000000400000;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
        *(.r*)
    }

    .data : {
        *(.data*)
        *(.d*)
    }

    /* Framebuffer info BSS sections */
    .bss : {
        *(.bss.fb_base)
        *(.bss.fb_width)
        *(.bss.fb_height)
        *(.bss.fb_pitch)
        *(.bss*)
        *(.b*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
    }
}
