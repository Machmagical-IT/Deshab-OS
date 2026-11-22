/*
 * Zratro OS - Fast System Call Entry (syscall/sysret)
 * 
 * 使用 SYSCALL/SYSRET 指令实现快速系统调用
 * 
 * 重要：用户线程进入内核后，在内核栈上构建 iretq 帧，
 * 以便调度器可以正确保存和恢复上下文。
 */

.section .text
.code64

/* ========== MSR 定义 ========== */
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_CSTAR       0xC0000083
#define MSR_SFMASK      0xC0000084
#define MSR_EFER        0xC0000080

/* ========== 系统调用入口 ========== */

.global syscall_entry
syscall_entry:
    /*
     * 用户态通过 syscall 进入这里
     * RCX = 返回地址 (用户 RIP)
     * R11 = 用户 RFLAGS
     * RAX = 系统调用号
     * RDI, RSI, RDX, R10, R8, R9 = 参数
     * 
     * 内核态：
     * CS = 0x08 (内核代码段)
     * SS = 0x10 (内核数据段)
     */
    
    /* 交换 GS (切换到内核 GS) */
    swapgs
    
    /* 保存用户栈指针，切换到内核栈 */
    mov %rsp, %gs:8          /* 保存用户 RSP 到 CPU 本地存储 */
    mov %gs:0, %rsp          /* 加载内核栈 */
    
    /* 禁用中断，防止 syscall 处理期间被抢占 */
    cli
    
    /* ========================================
     * 构建 iretq 帧 (以便调度器可以正确切换)
     * 
     * iretq 帧布局 (从高地址到低地址):
     *   SS    (用户数据段 = 0x23)
     *   RSP   (用户栈指针)
     *   RFLAGS
     *   CS    (用户代码段 = 0x1B)
     *   RIP   (返回地址)
     * ======================================== */
    
    /* 用户段选择子 */
    pushq $0x23              /* SS = 用户数据段 (0x20 | 3) */
    push %gs:8               /* 用户 RSP */
    push %r11                /* 用户 RFLAGS */
    pushq $0x1B              /* CS = 用户代码段 (0x18 | 3) */
    push %rcx                /* 返回地址 (用户 RIP) */
    
    /* 保存通用寄存器 */
    push %rax                /* 保存系统调用号/返回值 */
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    
    /*
     * 栈布局现在:
     *   RSP+0x00: r15
     *   RSP+0x08: r14
     *   RSP+0x10: r13
     *   RSP+0x18: r12
     *   RSP+0x20: rbp
     *   RSP+0x28: rbx
     *   RSP+0x30: rax (系统调用号)
     *   RSP+0x38: RIP
     *   RSP+0x40: CS
     *   RSP+0x48: RFLAGS
     *   RSP+0x50: 用户 RSP
     *   RSP+0x58: SS
     *
     * 注意：参数在 RDI, RSI, RDX, R10, R8, R9 中
     */
    
    /* 设置内核段寄存器 */
    mov $0x10, %bx
    mov %bx, %ds
    mov %bx, %es
    
    /* 参数调整：
     * Linux ABI: RDI, RSI, RDX, R10, R8, R9
     * C 调用约定: RDI, RSI, RDX, RCX, R8, R9
     * 所以 R10 需要移动到 RCX
     */
    mov %r10, %rcx
    
    /* ========================================
     * 预保存用户态 RIP 到线程 context
     *
     * 问题：syscall_dispatch 调用其他函数后，栈布局改变，
     *       context_switch 无法从栈上可靠读取用户态 RIP。
     *
     * 解决：在调用 syscall_dispatch 之前，使用 GS 寄存器获取当前线程指针，
     *       将 %rcx（用户态 RIP）保存到 thread->context.rip。
     *
     * GS 寄存器布局 (cpu_local 结构体):
     *   GS:0x30 = current_thread (struct thread *)
     *
     * 偏移量计算 (编译时 _Static_assert 验证):
     *   offsetof(struct thread, context) = 0x150
     *   offsetof(struct cpu_context, rip) = 0x78
     *   thread->context.rip = 0x150 + 0x78 = 0x1C8
     * ======================================== */
    mov %gs:0x30, %r11           /* 获取当前线程指针 (cpu_local.current_thread) */
    test %r11, %r11              /* 检查是否为 NULL */
    jz .Lskip_rip_save           /* 如果为 NULL，跳过保存 */
    mov %rcx, 0x1C8(%r11)        /* 保存用户态 RIP 到 thread->context.rip */

.Lskip_rip_save:
    
    /* 调用系统调用分发器
     * RAX = 系统调用号
     * RDI, RSI, RDX, RCX, R8, R9 = 参数
     */
    call syscall_dispatch
    
    /* 返回值在 RAX */
    
    /* 保存返回值到栈上 */
    mov %rax, 0x30(%rsp)     /* 更新保存的 RAX */
    
    /* 恢复通用寄存器 */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    pop %rax                 /* 恢复返回值 */
    
    /* 保存内核栈指针 (供下次进入内核时使用) */
    mov %rsp, %gs:0
    
    /* 重新启用中断 */
    sti
    
    /* 执行 iretq 返回用户态 */
    /* iretq 会弹出: RIP, CS, RFLAGS, RSP, SS */
    iretq

/* ========== 兼容模式系统调用 ========== */

.global syscall_entry_compat
syscall_entry_compat:
    /* 32 位兼容模式系统调用 */
    jmp syscall_entry

/* ========== MSR 操作函数 ========== */

.global wrmsr
wrmsr:
    wrmsr
    ret

.global rdmsr
rdmsr:
    xor %rdx, %rdx
    xor %rax, %rax
    rdmsr
    shl $32, %rdx
    or %rdx, %rax
    ret

/* ========== 初始化快速系统调用 ========== */

.global syscall_fast_init
syscall_fast_init:
    /* 设置 STAR 寄存器 */
    mov $MSR_STAR, %rcx
    mov $0x001B000800000000, %rax
    xor %rdx, %rdx
    wrmsr
    
    /* 设置 LSTAR (系统调用入口) */
    mov $MSR_LSTAR, %rcx
    lea syscall_entry(%rip), %rax
    mov %rax, %rdx
    shr $32, %rdx              /* RDX = 高 32 位 */
    /* RAX 低 32 位保持不变 */
    wrmsr
    
    /* 设置 SFMASK (保存的 RFLAGS) */
    mov $MSR_SFMASK, %rcx
    mov $0x200, %rax
    xor %rdx, %rdx
    wrmsr
    
    /* 启用 SYSCALL/SYSRET (EFER.SCE) */
    mov $MSR_EFER, %rcx
    rdmsr
    or $1, %eax
    wrmsr
    
    ret

/* ========== 传统中断系统调用入口 ========== */

.global syscall_int80_entry
syscall_int80_entry:
    push %rax
    
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    
    mov 8(%rsp), %rax
    mov %rbx, %rdi
    mov %rcx, %rsi
    mov %rdx, %rdx
    
    call syscall_dispatch
    
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    add $8, %rsp
    
    iretq

/* ========== CPU 本地存储布局 ========== */
/*
 * GS:0  = 内核栈指针
 * GS:8  = 用户栈指针
 * GS:16 = 当前线程指针
 * GS:24 = CPU 本地数据指针
 */
