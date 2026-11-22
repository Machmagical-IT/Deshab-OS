# IDT 中断处理入口 (64位) - GAS 语法

.section .text
.code64

# 外部全局变量（在 syscall.c 中定义）
.extern syscall_debug_rdi
.extern syscall_debug_rsi
.extern syscall_debug_rdx
.extern syscall_debug_rax

# 通用中断处理程序
.extern isr_handler

# 通用中断存根 (保存寄存器，调用处理程序，恢复寄存器)
.align 16
isr_common_stub:
    # 保存所有通用寄存器
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
    
    # 传递栈帧指针给 C 函数
    mov %rsp, %rdi
    
    # 调用 C 处理函数
    call isr_handler
    
    # 恢复通用寄存器
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
    pop %rax
    
    # 清理错误码和中断号
    add $16, %rsp
    
    # 恢复中断 (RFLAGS 中 IF 位)
    sti
    
    iretq

# CPU 异常入口 (无错误码)
.macro ISR_NOERR num
.global isr_\num
isr_\num:
    pushq $0              # 压入伪错误码
    pushq $\num           # 压入中断号
    jmp isr_common_stub
.endm

# CPU 异常入口 (有错误码)
.macro ISR_ERR num
.global isr_\num
isr_\num:
    pushq $\num           # 压入中断号 (错误码由 CPU 压入)
    jmp isr_common_stub
.endm

# CPU 异常 (向量 0-31)
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

# IRQ 入口 (向量 32-47)
.macro IRQ_HANDLER num
.global irq_\num
irq_\num:
    pushq $0
    pushq $(32 + \num)
    jmp isr_common_stub
.endm

IRQ_HANDLER 0
IRQ_HANDLER 1
IRQ_HANDLER 2
IRQ_HANDLER 3
IRQ_HANDLER 4
IRQ_HANDLER 5
IRQ_HANDLER 6
IRQ_HANDLER 7
IRQ_HANDLER 8
IRQ_HANDLER 9
IRQ_HANDLER 10
IRQ_HANDLER 11
IRQ_HANDLER 12
IRQ_HANDLER 13
IRQ_HANDLER 14
IRQ_HANDLER 15

# APIC 定时器中断 (向量 48)
.global apic_timer_isr
apic_timer_isr:
    pushq $0
    pushq $48
    jmp isr_common_stub

# APIC 错误中断 (向量 49)
.global apic_error_isr
apic_error_isr:
    pushq $0
    pushq $49
    jmp isr_common_stub

# APIC 伪中断 (向量 255 - 必须是 255)
.global apic_spurious_isr
apic_spurious_isr:
    pushq $0
    pushq $255
    jmp isr_common_stub

# IPI 中断入口 (向量 0x60-0x65)
# IPI_RESCHEDULE (0x60)
.global ipi_reschedule_isr
ipi_reschedule_isr:
    pushq $0
    pushq $0x60
    jmp isr_common_stub

# IPI_CALL_FUNCTION (0x61)
.global ipi_call_function_isr
ipi_call_function_isr:
    pushq $0
    pushq $0x61
    jmp isr_common_stub

# IPI_CPU_STOP (0x62)
.global ipi_cpu_stop_isr
ipi_cpu_stop_isr:
    pushq $0
    pushq $0x62
    jmp isr_common_stub

# IPI_CPU_START (0x63)
.global ipi_cpu_start_isr
ipi_cpu_start_isr:
    pushq $0
    pushq $0x63
    jmp isr_common_stub

# IPI_TLB_SHOOTDOWN (0x64)
.global ipi_tlb_shootdown_isr
ipi_tlb_shootdown_isr:
    pushq $0
    pushq $0x64
    jmp isr_common_stub

# IPI_MEM_BARRIER (0x65)
.global ipi_mem_barrier_isr
ipi_mem_barrier_isr:
    pushq $0
    pushq $0x65
    jmp isr_common_stub

# 系统调用入口
.global syscall_isr
syscall_isr:
    cli                     # 禁用中断，防止 syscall 处理期间被抢占
    # 保存 syscall 参数到全局变量（用于调试）
    mov %rdi, syscall_debug_rdi
    mov %rsi, syscall_debug_rsi
    mov %rdx, syscall_debug_rdx
    mov %rax, syscall_debug_rax
    pushq $0
    pushq $0x80
    jmp isr_common_stub

# 加载 IDT
.global idt_load
idt_load:
    lidt (%rdi)
    ret
