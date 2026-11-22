/*
 * Zratro OS - AP (Application Processor) Startup Assembly
 * 
 * 此代码运行在实模式，被复制到低地址执行
 */

.section .text.ap_trampoline, "ax"
.code16

.global ap_start
ap_start:
    /* 禁用中断 */
    cli
    
    /* 设置段寄存器 */
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    
    /* 启用 A20 地址线 */
    in $0x92, %al
    or $0x02, %al
    out %al, $0x92
    
    /* 加载 GDT (临时) */
    lgdt ap_gdt64_desc - ap_start
    
    /* 设置 PE 和 PAE 位 */
    mov %cr4, %eax
    or $0x620, %eax      /* PAE | PGE | OSFXSR */
    mov %eax, %cr4
    
    /* 设置页表 */
    mov 0x5010, %eax     /* boot_info->pml4 */
    mov %eax, %cr3
    
    /* 启用长模式 */
    mov $0xC0000080, %ecx
    rdmsr
    or $0x1000, %eax     /* LME */
    wrmsr
    
    /* 启用分页和保护模式 */
    mov %cr0, %eax
    or $0x80000001, %eax  /* PG | PE */
    mov %eax, %cr0
    
    /* 加载 64 位代码段 */
    ljmp $0x08, $ap_long_mode - ap_start + 0x5000

.code64
ap_long_mode:
    /* 设置段寄存器 */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    
    /* 加载栈 */
    movq 0x5018, %rsp    /* boot_info->stack (64-bit) */
    
    /* 设置 GS 段基址 */
    movq 0x5020, %rdi    /* boot_info->cpu_local (64-bit) */
    mov %rdi, %rdx
    shr $32, %rdx
    mov $0xC0000101, %ecx   /* IA32_GS_BASE */
    wrmsr
    
    /* 跳转到 C 代码 */
    mov 0x5008, %rax    /* boot_info->kernel_entry */
    call *%rax
    
    /* 不应该到达这里 */
    cli
1:  hlt
    jmp 1b

/* 临时 GDT */
.align 16
ap_gdt64:
    /* 空描述符 */
    .quad 0
    /* 代码段 */
    .quad 0x00AF9A000000FFFF
    /* 数据段 */
    .quad 0x00CF92000000FFFF
ap_gdt64_end:

ap_gdt64_desc:
    .word ap_gdt64_end - ap_gdt64 - 1
    .quad ap_gdt64 - ap_start + 0x5000

.section .text
.code64

/* AP 主函数入口 (供 smp.c 调用) */
.global ap_entry
ap_entry:
    /* 此函数由 ap_trampoline 跳转 */
    /* 已经在 64 位模式 */
    call ap_main
    
    /* 如果返回，进入无限循环 */
    cli
1:  hlt
    jmp 1b
