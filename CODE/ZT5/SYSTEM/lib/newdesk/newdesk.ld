/* newdesk 链接脚本 - 生成固定地址的 ELF EXEC 格式 */
/* 与旧 desk (C) 保持一致的内存布局，确保内核 ELF 加载器能正确加载 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

/* 帧缓冲区信息放置在 0x800000 (8MB)，内核通过 SYS_GETFBINFO 填充 */
USER_FB_INFO = 0x0000000000800000;

SECTIONS
{
    /* 代码段从 0x400000 (4MB) 开始，与旧 desk 一致 */
    . = 0x0000000000400000;

    .text : {
        *(.text .text.*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }

    .data : ALIGN(4096) {
        *(.data .data.*)
        *(.sdata .sdata.*)
        /* 保留 GOT 段（relocation-model=static 时通常为空，但链接器需要） */
        *(.got)
        *(.got.plt)
        *(.data.rel.ro .data.rel.ro.*)
    }

    /* BSS 段 */
    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* 丢弃不需要的段 */
    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.comment)
        *(.note.*)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.gnu.hash)
        *(.hash)
        *(.rela.*)
        *(.plt)
        *(.plt.got)
    }
}
