/*
 * C 运行时启动代码 (crt0) - Zratro Shell
 *
 * 这是 shell 程序的入口点，在 main() 之前执行
 */

.global _start
.extern main

.section .text

_start:
    /* 设置栈帧 */
    xor    %rbp, %rbp

    /* argc 在栈顶, argv 在 argc 之后 */
    mov    (%rsp), %rdi          /* argc */
    lea    8(%rsp), %rsi         /* argv */

    /* 调用 main(argc, argv) */
    call   main

    /* main() 返回值在 %rax 中，作为 exit 参数 */
    mov    %rax, %rdi

    /* 调用 exit syscall (Zratro SYS_exit = 0)
     * 使用 int 0x80 而不是 syscall 指令，因为 %gs 未初始化 */
    xor    %rax, %rax            /* SYS_exit = 0 */
    int    $0x80

    /* 不应该返回 */
    jmp    .
