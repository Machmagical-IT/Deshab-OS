/*
 * Zratro OS - Windows x64 调用约定转换 Thunk
 *
 * Windows x64 调用约定: RCX, RDX, R8, R9 (前4个参数)
 * System V x64 调用约定: RDI, RSI, RDX, RCX (前4个参数)
 *
 * PE 程序通过 IAT 调用兼容层函数时，使用 Microsoft x64 约定传参。
 * 这些 thunk 将参数从 Windows 约定转换为 System V 约定后跳转到实际实现。
 *
 * 注意：这些 thunk 用于 IAT 填充，不是直接调用的函数。
 * 在 pe.c 的 find_builtin_export 中，返回 thunk 地址而非直接实现地址。
 */

.section .text

/* 通用 4 参数 thunk: Windows (RCX,RDX,R8,R9) -> System V (RDI,RSI,RDX,RCX) */
.global win_thunk_4args
win_thunk_4args:
    mov %rcx, %rdi    /* arg1: RCX -> RDI */
    mov %rdx, %rsi    /* arg2: RDX -> RSI */
    mov %r8, %rdx     /* arg3: R8  -> RDX */
    mov %r9, %rcx     /* arg4: R9  -> RCX */
    jmp *%rax         /* 跳转到实际实现（RAX 由调用者设置） */

/* 通用 3 参数 thunk: Windows (RCX,RDX,R8) -> System V (RDI,RSI,RDX) */
.global win_thunk_3args
win_thunk_3args:
    mov %rcx, %rdi    /* arg1: RCX -> RDI */
    mov %rdx, %rsi    /* arg2: RDX -> RSI */
    mov %r8, %rdx     /* arg3: R8  -> RDX */
    jmp *%rax         /* 跳转到实际实现 */

/* 通用 2 参数 thunk: Windows (RCX,RDX) -> System V (RDI,RSI) */
.global win_thunk_2args
win_thunk_2args:
    mov %rcx, %rdi    /* arg1: RCX -> RDI */
    mov %rdx, %rsi    /* arg2: RDX -> RSI */
    jmp *%rax         /* 跳转到实际实现 */

/* 通用 1 参数 thunk: Windows (RCX) -> System V (RDI) */
.global win_thunk_1arg
win_thunk_1arg:
    mov %rcx, %rdi    /* arg1: RCX -> RDI */
    jmp *%rax         /* 跳转到实际实现 */

/* 0 参数 thunk: 直接跳转 */
.global win_thunk_0args
win_thunk_0args:
    jmp *%rax         /* 跳转到实际实现 */

/*
 * 7 参数 thunk: Windows (RCX,RDX,R8,R9,栈5,栈6,栈7)
 *             -> System V (RDI,RSI,RDX,RCX,R8,R9,栈7)
 *
 * Windows x64: 前4个在寄存器，第5-7个在调用者栈上 [rsp+0x28], [rsp+0x30], [rsp+0x38]
 * System V:    前6个在寄存器，第7个在调用者栈上 [rsp+0x8]
 */
.global win_thunk_7args
win_thunk_7args:
    /* 保存返回地址 */
    mov (%rsp), %r11
    /* 调整栈：Windows 第5-7参数 -> System V 第7参数 */
    /* Windows: [rsp+0x28]=arg5, [rsp+0x30]=arg6, [rsp+0x38]=arg7 */
    /* System V: R8=arg5, R9=arg6, [rsp+0x8]=arg7 */
    mov %rcx, %rdi    /* arg1 */
    mov %rdx, %rsi    /* arg2 */
    mov %r8, %rdx     /* arg3 */
    mov %r9, %rcx     /* arg4 */
    mov 0x28(%rsp), %r8  /* arg5: 从栈上取 */
    mov 0x30(%rsp), %r9  /* arg6: 从栈上取 */
    /* 第7个参数：从 Windows [rsp+0x38] 移到 System V [rsp+0x8] */
    mov 0x38(%rsp), %r10
    sub $0x20, %rsp      /* 腾出 shadow space */
    mov %r10, 0x8(%rsp)  /* 第7个参数 */
    push %r11            /* 恢复返回地址 */
    jmp *%rax

/*
 * 5 参数 thunk: Windows (RCX,RDX,R8,R9,栈5)
 *             -> System V (RDI,RSI,RDX,RCX,R8)
 */
.global win_thunk_5args
win_thunk_5args:
    mov %rcx, %rdi
    mov %rdx, %rsi
    mov %r8, %rdx
    mov %r9, %rcx
    mov 0x28(%rsp), %r8  /* arg5: 从 Windows 栈上取 */
    jmp *%rax

/*
 * 6 参数 thunk: Windows (RCX,RDX,R8,R9,栈5,栈6)
 *             -> System V (RDI,RSI,RDX,RCX,R8,R9)
 */
.global win_thunk_6args
win_thunk_6args:
    mov %rcx, %rdi
    mov %rdx, %rsi
    mov %r8, %rdx
    mov %r9, %rcx
    mov 0x28(%rsp), %r8  /* arg5 */
    mov 0x30(%rsp), %r9  /* arg6 */
    jmp *%rax
