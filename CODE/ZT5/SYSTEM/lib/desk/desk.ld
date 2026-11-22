/* User program linker script for desk */
/* This script places framebuffer and logo variables at fixed addresses
   so the kernel can reliably write to them without needing address updates */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

/* Framebuffer info will be placed at 0x800000 (8MB) in user space */
USER_FB_INFO = 0x0000000000800000;

SECTIONS
{
    /* Start code at fixed address */
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
    
    /* BSS section */
    .bss : {
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
