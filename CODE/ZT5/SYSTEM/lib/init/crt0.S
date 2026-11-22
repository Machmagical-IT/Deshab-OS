/*
 * C 运行时启动代码 (crt0)
 * 
 * 这是用户程序的入口点，在 main() 之前执行
 * 职责:
 *   1. 设置栈帧
 *   2. 初始化全局变量 (.bss 清零)
 *   3. 调用 main()
 *   4. 退出程序
 */

.global _start
.extern main

.section .text

_start:
    /* 设置栈帧 */
    xor    %rbp, %rbp
    
    /* 清零所有通用寄存器（避免垃圾数据）*/
    xor    %rax, %rax
    xor    %rbx, %rbx
    xor    %rcx, %rcx
    xor    %rdx, %rdx
    xor    %r8, %r8
    xor    %r9, %r9
    xor    %r10, %r10
    xor    %r11, %r11
    xor    %r12, %r12
    xor    %r13, %r13
    xor    %r14, %r14
    xor    %r15, %r15
    
    /* argc 在栈顶, argv 在 argc 之后 
     * 注意：不修改 RSP，直接使用内核设置的栈
     * 内核已经保证 16 字节对齐
     */
    mov    (%rsp), %rdi          /* argc */
    lea    8(%rsp), %rsi         /* argv */
    
    /* 清零 .bss 段 (如果需要) */
    /* 这里简化处理，假设加载器已经清零 */
    
    /* 调用 main(argc, argv) */
    call   main
    
    /* main() 返回值在 %rax 中 */
    mov    %rax, %rdi
    
    /* 调用 exit() */
    mov    $0, %rax              /* SYS_EXIT */
    int    $0x80
    
    /* 不应该返回，但如果返回则无限循环 */
    jmp    .

.section .bss
    /* 未初始化数据 */
