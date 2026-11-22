# GDT 和 TSS 加载函数 (64位) - GAS 语法

.section .text
.code64

# void gdt_load(struct gdt_ptr *gdt_ptr)
.global gdt_load
gdt_load:
    lgdt (%rdi)          # 加载 GDT (第一个参数在 RDI)
    
    # 重载段寄存器
    # 注意：gs 不在这里设置！
    # gs 的基址应该通过 IA32_GS_BASE MSR 设置，由 smp_init() 完成
    # 这样 gs:0x28 才能正确访问 cpu_data[cpu_id].stack_canary
    mov $0x10, %ax       # 内核数据段选择子
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    # gs 保持原样，等待 smp_init() 通过 wrmsr 设置
    mov %ax, %ss
    
    # 重载代码段 (使用 far return)
    pop %rdi             # 保存返回地址
    mov $0x08, %rax      # 内核代码段选择子
    push %rax
    push %rdi
    lretq
    
    ret

# void tss_load(uint16_t selector)
.global tss_load
tss_load:
    ltr %di              # 加载任务寄存器
    ret
