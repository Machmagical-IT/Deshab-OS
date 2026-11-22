OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    limine_reqs PT_LOAD;
    text        PT_LOAD;
    rodata      PT_LOAD;
    data        PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;

    /* Limine 请求段 - 必须对齐到 8 字节 */
    .limine_reqs ALIGN(8) : {
        KEEP(*(.limine_reqs_start))
        KEEP(*(.limine_reqs))
        KEEP(*(.limine_reqs_end))
    } :limine_reqs

    . = ALIGN(4096);

    /* 初始化代码段 - __init 函数放在这里，启动后可释放 */
    .init.text : {
        *(.init.text)
    } :text

    /* 退出代码段 - __exit 函数 */
    .exit.text : {
        *(.exit.text)
    } :text

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(4096);

    .rodata : {
        *(.rodata .rodata.*)
        /* 异常表 - 用于 copy_to_user/copy_from_user 的页错误恢复 */
        . = ALIGN(8);
        __ex_table_start = .;
        *(__ex_table)
        __ex_table_end = .;
    } :rodata

    . = ALIGN(4096);

    /* 读多写少数据段 - 优化缓存局部性 */
    .data..read_mostly : {
        *(.data..read_mostly)
    } :data

    /* 缓存行对齐数据段 */
    .data..cacheline_aligned : ALIGN(64) {
        *(.data..cacheline_aligned)
    } :data

    .data : {
        *(.data .data.*)
    } :data

    . = ALIGN(4096);

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    . = ALIGN(4096);

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
    }
}
