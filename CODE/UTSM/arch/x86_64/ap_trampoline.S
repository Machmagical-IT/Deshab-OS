/* ap_trampoline.S — 把 AP 从 SIPI 实模式带入 64 位，跳入 C 入口。
 *
 * 本 blob 被 smp.c 从高半区复制到物理 0x8000（恒等映射页）。
 * SIPI vector=0x08 → CS=0x0800, IP=0 → 物理 0x8000。
 * 参数块固定放在 0x8F00（同页尾部）：
 *   +0x08 cr3 (u64) / +0x10 stack_top (u64) / +0x18 entry (u64)
 *   +0x20 arg (u64 → RSI) / +0x28 cpu index (u32 → RDI)
 *
 * 关键点（R5 实机/仿真定位所得，勿回退）：
 *   1. 所有绝对地址用 "0x8000 + (label - start)"，避免链接器编码高半区地址；
 *      但 32 位跳转目标不能重复加 AP_BASE —— EBX 已含 CS<<4 = 0x8000。
 *   2. 必须先置 CR0.PE=1，far jump 才会按 GDT 选择子解释；否则变成实模式
 *      物理跳转（0x08<<4 + offset），跑进垃圾内存后再绕回 0x8000。
 *   3. GDT 必须分两段：32 位阶段只能用 L=0 的代码段；L=1 的 64 位代码段
 *      只有在开启分页（IA-32e 生效）后才能加载，否则 #GP → 三重复位。
 *   4. 装载 entry 到 %rax 之后不得再写 %al（串口标记会破坏低 8 位）。
 */

#define AP_BASE        0x8000
#define AP_PARAM       0x8F00

    .section .text
    .code16
    .global ap_trampoline_start
    .global ap_trampoline_end
ap_trampoline_start:
    cli
    cld
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movl    $0x7F00, %esp

    /* 加载内嵌 GDT（绝对线性地址） */
    lgdtl   (AP_BASE + (ap_gdt_desc - ap_trampoline_start))

    /* CR4.PAE */
    movl    %cr4, %eax
    orl     $0x20, %eax
    movl    %eax, %cr4

    /* EFER.LME */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $0x100, %eax
    wrmsr

    /* CR0.PE = 1（先在保护模式下完成 far jump，见文件头第 2 点） */
    movl    %cr0, %eax
    orl     $0x00000001, %eax
    movl    %eax, %cr0

    /* 进入 32 位保护模式（CS=0x08，L=0 的 32 位代码段） */
    movw    %cs, %ax
    shlw    $4, %ax
    movzwl  %ax, %ebx
    leal    (ap_trampoline_32 - ap_trampoline_start)(%ebx), %eax
    pushl   $0x08
    pushl   %eax
    lretl

    .code32
ap_trampoline_32:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    /* CR3（低 32 位；高 32 位由 smp.c 预检为 0） */
    movl    (AP_PARAM + 0x08), %eax
    movl    %eax, %cr3

    /* CR0.PG | PE */
    movl    %cr0, %eax
    orl     $0x80000001, %eax
    movl    %eax, %cr0

    /* 进入 64 位代码段（CS=0x18，L=1） */
    movl    $(AP_BASE + (ap_trampoline_64 - ap_trampoline_start)), %eax
    pushl   $0x18
    pushl   %eax
    lretl

    .code64
ap_trampoline_64:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    movq    (AP_PARAM + 0x10), %rsp
    xorq    %rbp, %rbp
    movq    (AP_PARAM + 0x18), %rax      /* entry */
    movq    (AP_PARAM + 0x20), %rsi      /* arg */
    movq    (AP_PARAM + 0x28), %rdi      /* cpu index → RDI */
    jmp     *%rax

    .align 8
ap_gdt:
    .quad   0x0000000000000000      /* 0x00 null */
    .quad   0x00CF9A000000FFFF      /* 0x08 32 位代码（L=0,D=1） */
    .quad   0x00CF92000000FFFF      /* 0x10 flat data */
    .quad   0x00AF9A000000FFFF      /* 0x18 64 位代码（L=1,D=0） */
ap_gdt_end:

ap_gdt_desc:
    .word   (ap_gdt_end - ap_gdt - 1)
    .long   (AP_BASE + (ap_gdt - ap_trampoline_start))

ap_trampoline_end:
    .code64
