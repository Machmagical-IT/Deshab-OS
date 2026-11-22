/*
 * Zratro OS - CPU Context Switch Assembly
 */

.global context_switch
.global co_switch
.global co_fswap

.extern co_entry

/*
 * context_switch(old_context, new_context)
 */
context_switch:
    /* 保存返回地址到 r11 (临时) */
    mov     (%rsp), %r11
    
    /* 检查新线程的 rip 是否在内核地址范围 */
    mov     0x78(%rsi), %rax
    shr     $48, %rax
    cmp     $0xFFFF, %ax
    je      .Lkernel_thread
    
/* ========== 用户态线程 ========== */
.Luser_thread:
    /* 用户态线程：CS=0x1B (用户代码+RPL3), SS=0x23 (用户数据+RPL3) */
    mov     $0x1B, %rax
    mov     %rax, 0x80(%rsi)     /* 强制设置 CS */
    mov     $0x23, %rax
    mov     %rax, 0x98(%rsi)     /* 强制设置 SS */
    jmp     .Lrestore_common
    
/* ========== 内核线程 ========== */
.Lkernel_thread:
    /* 内核线程：CS=0x08 (内核代码), SS=0x10 (内核数据) */
    mov     $0x08, %rax
    mov     %rax, 0x80(%rsi)     /* 强制设置 CS */
    mov     $0x10, %rax
    mov     %rax, 0x98(%rsi)     /* 强制设置 SS */
    
.Lrestore_common:
    /* 保存旧线程的通用寄存器 */
    mov     %r15, 0x00(%rdi)
    mov     %r14, 0x08(%rdi)
    mov     %r13, 0x10(%rdi)
    mov     %r12, 0x18(%rdi)
    /* r11 稍后保存 */
    mov     %r10, 0x28(%rdi)
    mov     %r9,  0x30(%rdi)
    mov     %r8,  0x38(%rdi)
    mov     %rdi, 0x40(%rdi)
    mov     %rsi, 0x48(%rdi)
    mov     %rbp, 0x50(%rdi)
    mov     %rbx, 0x58(%rdi)
    mov     %rdx, 0x60(%rdi)
    mov     %rcx, 0x68(%rdi)
    mov     %rax, 0x70(%rdi)
    
    /* 现在保存 r11 */
    mov     %r11, 0x20(%rdi)      /* old->r11 */
    
    /* 保存 RSP (跳过返回地址)
     * 对于用户线程：也需要保存 RSP，因为 schedule() 在内核态调用，
     * 线程被唤醒后需要从 schedule() 返回点继续执行，而不是重新执行 iretq
     * 对于内核线程：保存 RSP 以恢复执行 */
    lea     0x08(%rsp), %rax
    mov     %rax, 0x90(%rdi)
    
    /* 保存 RFLAGS — 强制设置 IF=1，确保线程被恢复时中断启用 */
    pushf
    orq     $0x200, (%rsp)       /* RFLAGS.IF = 1 */
    pop     %rax
    mov     %rax, 0x88(%rdi)
    
    /* ========================================
     * 保存旧线程的 RIP
     *
     * 使用返回地址（%r11 = context_switch 的返回地址）来判断：
     * - 如果返回地址在内核地址范围（0xFFFFxxxx），说明线程在内核态被切走，
     *   设置 rip 为返回地址，线程被唤醒后从 schedule() 返回点继续执行
     * - 如果返回地址在用户态范围，说明线程通过其他方式被切走，
     *   保持 context.rip 不变
     * ======================================== */
    mov     %r11, %rax
    shr     $48, %rax
    cmp     $0xFFFF, %ax
    jne     .Lold_thread_done    /* 用户态地址，保持 context.rip 不变 */

    /* 内核地址：rip 设置为返回地址 */
    mov     %r11, 0x78(%rdi)      /* old->rip = 返回地址 */
    
.Lold_thread_done:
    
    /* CS/SS 已在上面强制设置，不再从硬件寄存器读取 */
    
    /* ===== 恢复新线程 ===== */
    
    /* 检查新线程的 rip 是否在内核地址范围 (再次检查) */
    mov     0x78(%rsi), %rax
    shr     $48, %rax
    cmp     $0xFFFF, %ax
    je      .Lkernel_thread_exec
    
/* ========== 用户态线程恢复 ========== */
.Luser_thread_restore:
    /* 恢复 r8-r15 (先恢复这些，构建 iretq 帧时需要用到 %rax) */
    mov     (%rsi), %r15
    mov     0x08(%rsi), %r14
    mov     0x10(%rsi), %r13
    mov     0x18(%rsi), %r12
    mov     0x20(%rsi), %r11
    mov     0x28(%rsi), %r10
    mov     0x30(%rsi), %r9
    mov     0x38(%rsi), %r8

    /* 构建 iretq 栈帧: SS, RSP, RFLAGS, CS, RIP
     * 用户态线程使用用户段选择子，RSP 使用 user_rsp (偏移 0xA0)
     * 这与 .Lkernel_thread_exec 路径一致，不再依赖 ELF 加载器预构建的帧 */
    mov     $0x23, %rax              /* SS = 用户数据段 0x23 */
    push    %rax
    mov     0xA0(%rsi), %rax         /* user_rsp (偏移 0xA0) */
    push    %rax
    mov     0x88(%rsi), %rax         /* RFLAGS */
    push    %rax
    mov     $0x1B, %rax              /* CS = 用户代码段 0x1B */
    push    %rax
    mov     0x78(%rsi), %rax         /* RIP */
    push    %rax

    /* 恢复其他寄存器 */
    mov     0x50(%rsi), %rbp
    mov     0x58(%rsi), %rbx
    mov     0x40(%rsi), %rdi
    mov     0x60(%rsi), %rdx
    mov     0x68(%rsi), %rcx
    mov     0x70(%rsi), %rax
    mov     0x48(%rsi), %rsi

    iretq
    
/* ========== 内核线程恢复 ========== */
.Lkernel_thread_exec:
    mov     (%rsi), %r15
    mov     0x08(%rsi), %r14
    mov     0x10(%rsi), %r13
    mov     0x18(%rsi), %r12
    mov     0x20(%rsi), %r11
    mov     0x28(%rsi), %r10
    mov     0x30(%rsi), %r9
    mov     0x38(%rsi), %r8
    
    /* 构建 iretq 栈帧: SS, RSP, RFLAGS, CS, RIP */
    /* 内核线程使用固定的内核段选择子 */
    mov     $0x10, %rax              /* SS = 内核数据段 0x10 */
    push    %rax
    mov     0x90(%rsi), %rax         /* RSP */
    push    %rax
    mov     0x88(%rsi), %rax         /* RFLAGS */
    push    %rax
    mov     $0x08, %rax              /* CS = 内核代码段 0x08 */
    push    %rax
    mov     0x78(%rsi), %rax         /* RIP */
    push    %rax
    
    /* 恢复其他寄存器 */
    mov     0x50(%rsi), %rbp
    mov     0x58(%rsi), %rbx
    mov     0x40(%rsi), %rdi
    mov     0x60(%rsi), %rdx
    mov     0x68(%rsi), %rcx
    mov     0x70(%rsi), %rax
    mov     0x48(%rsi), %rsi
    
    iretq

co_switch:
    test    %rdi, %rdi
    je      1f
    mov     %rbx, (%rdi)
    mov     %rbp, 0x08(%rdi)
    mov     %r12, 0x10(%rdi)
    mov     %r13, 0x18(%rdi)
    mov     %r14, 0x20(%rdi)
    mov     %r15, 0x28(%rdi)
    mov     %rsp, 0x30(%rdi)
    mov     (%rsp), %rax
    mov     %rax, 0x38(%rdi)
1:
    mov     (%rsi), %rbx
    mov     0x08(%rsi), %rbp
    mov     0x10(%rsi), %r12
    mov     0x18(%rsi), %r13
    mov     0x20(%rsi), %r14
    mov     0x28(%rsi), %r15
    mov     0x30(%rsi), %rsp
    jmp     *0x38(%rsi)

.global co_entry_wrapper
co_entry_wrapper:
    mov     (%rsp), %rdi
    mov     0x08(%rsp), %rsi
    call    co_entry
    cli
    hlt
    jmp     .

/*
 * co_fswap(struct co_fast_context *from, struct co_fast_context *to)
 *
 * struct co_fast_context layout:
 *   offset 0:  rsp
 *   offset 8:  rbp
 *   offset 16: rbx
 *   offset 24: r12
 *   offset 32: r13
 *   offset 40: r14
 *   offset 48: r15
 *   offset 56: rip
 *
 * Saves callee-saved registers to 'from', restores from 'to'.
 * If from is NULL, skip saving.
 */
co_fswap:
    /* rdi = from, rsi = to */
    test    %rdi, %rdi
    je      .Lfswap_skip_save

    /* Save callee-saved registers to from */
    mov     %rsp, 0x00(%rdi)
    mov     %rbp, 0x08(%rdi)
    mov     %rbx, 0x10(%rdi)
    mov     %r12, 0x18(%rdi)
    mov     %r13, 0x20(%rdi)
    mov     %r14, 0x28(%rdi)
    mov     %r15, 0x30(%rdi)
    /* Save return address as rip */
    mov     (%rsp), %rax
    mov     %rax, 0x38(%rdi)

.Lfswap_skip_save:
    /* Restore callee-saved registers from to */
    mov     0x00(%rsi), %rsp
    mov     0x08(%rsi), %rbp
    mov     0x10(%rsi), %rbx
    mov     0x18(%rsi), %r12
    mov     0x20(%rsi), %r13
    mov     0x28(%rsi), %r14
    mov     0x30(%rsi), %r15
    /* Jump to saved rip */
    jmp     *0x38(%rsi)
