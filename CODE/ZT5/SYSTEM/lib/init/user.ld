/* 用户程序链接脚本 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    /* 用户空间起始地址 (留出空间给 NULL 页) */
    . = 0x400000;
    
    /* 代码段 */
    .text : {
        *(.text .text.*)
    }
    
    /* 只读数据 */
    .rodata : {
        *(.rodata .rodata.*)
    }
    
    /* 数据段 */
    .data : {
        *(.data .data.*)
    }
    
    /* BSS 段 */
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }
    
    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
}
