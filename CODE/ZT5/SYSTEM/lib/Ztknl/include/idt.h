#ifndef IDT_H
#define IDT_H

#include <stdint.h>
#include <stddef.h>  /* For offsetof */

/* IDT 表项数量 */
#define IDT_ENTRIES 256

/* ========== 中断向量号定义 ========== */

/* CPU 异常 (0-31) */
#define INT_DE      0   /* #DE - 除法错误 (Divide Error) */
#define INT_DB      1   /* #DB - 调试异常 (Debug) */
#define INT_NMI     2   /* NMI - 不可屏蔽中断 */
#define INT_BP      3   /* #BP - 断点 (Breakpoint) */
#define INT_OF      4   /* #OF - 溢出 (Overflow) */
#define INT_BR      5   /* #BR - BOUND 范围超出 (Bound Range Exceeded) */
#define INT_UD      6   /* #UD - 无效操作码 (Invalid Opcode) */
#define INT_NM      7   /* #NM - 设备不可用 (Device Not Available) */
#define INT_DF      8   /* #DF - 双重错误 (Double Fault) */
#define INT_CSO     9   /* 协处理器段超出 (已废弃) */
#define INT_TS      10  /* #TS - 无效 TSS (Invalid TSS) */
#define INT_NP      11  /* #NP - 段不存在 (Segment Not Present) */
#define INT_SS      12  /* #SS - 栈段错误 (Stack-Segment Fault) */
#define INT_GP      13  /* #GP - 一般保护错误 (General Protection) */
#define INT_PF      14  /* #PF - 页错误 (Page Fault) */
#define INT_RSVD    15  /* 保留 */
#define INT_MF      16  /* #MF - x87 FPU 浮点错误 (x87 FPU Error) */
#define INT_AC      17  /* #AC - 对齐检查 (Alignment Check) */
#define INT_MC      18  /* #MC - 机器检查 (Machine Check) */
#define INT_XM      19  /* #XM - SIMD 浮点异常 (SIMD Exception) */
#define INT_VE      20  /* #VE - 虚拟化异常 (Virtualization Exception) */
#define INT_CP      21  /* #CP - 控制保护异常 (Control Protection) */

/* 异常名称字符串 */
#define INT_DE_NAME "Divide Error"
#define INT_DB_NAME "Debug Exception"
#define INT_NMI_NAME "NMI Interrupt"
#define INT_BP_NAME "Breakpoint"
#define INT_OF_NAME "Overflow"
#define INT_BR_NAME "BOUND Range Exceeded"
#define INT_UD_NAME "Invalid Opcode"
#define INT_NM_NAME "Device Not Available"
#define INT_DF_NAME "Double Fault"
#define INT_TS_NAME "Invalid TSS"
#define INT_NP_NAME "Segment Not Present"
#define INT_SS_NAME "Stack-Segment Fault"
#define INT_GP_NAME "General Protection Fault"
#define INT_PF_NAME "Page Fault"
#define INT_MF_NAME "x87 FPU Error"
#define INT_AC_NAME "Alignment Check"
#define INT_MC_NAME "Machine Check"
#define INT_XM_NAME "SIMD Exception"

/* 外部中断向量 (从 32 开始) */
#define IRQ_BASE        32
#define IRQ_TIMER       (IRQ_BASE + 0)   /* IRQ0 - 定时器 */
#define IRQ_KEYBOARD    (IRQ_BASE + 1)   /* IRQ1 - 键盘 */
#define IRQ_CASCADE     (IRQ_BASE + 2)   /* IRQ2 - 级联 */
#define IRQ_COM2        (IRQ_BASE + 3)   /* IRQ3 - 串口2 */
#define IRQ_COM1        (IRQ_BASE + 4)   /* IRQ4 - 串口1 */
#define IRQ_LPT2        (IRQ_BASE + 5)   /* IRQ5 - 并口2 */
#define IRQ_FLOPPY      (IRQ_BASE + 6)   /* IRQ6 - 软驱 */
#define IRQ_LPT1        (IRQ_BASE + 7)   /* IRQ7 - 并口1 */
#define IRQ_RTC         (IRQ_BASE + 8)   /* IRQ8 - RTC */
#define IRQ_ACPI        (IRQ_BASE + 9)   /* IRQ9 - ACPI */
#define IRQ_MOUSE       (IRQ_BASE + 12)  /* IRQ12 - 鼠标 */
#define IRQ_FPU         (IRQ_BASE + 13)  /* IRQ13 - FPU */
#define IRQ_ATA1        (IRQ_BASE + 14)  /* IRQ14 - IDE 主 */
#define IRQ_ATA2        (IRQ_BASE + 15)  /* IRQ15 - IDE 从 */

/* APIC 中断向量 */
#define APIC_TIMER_VECTOR       48
#define APIC_ERROR_VECTOR       49
#define APIC_THERMAL_VECTOR     50
#define APIC_PERF_VECTOR        51
#define APIC_LINT0_VECTOR       52
#define APIC_LINT1_VECTOR       53
#define APIC_SPURIOUS_VECTOR    255

/* 系统调用向量 */
#define SYSCALL_VECTOR          0x80
#define SYSCALL_FAST_VECTOR     0x81    /* 快速系统调用 */

/* IPI 向量定义在 smp.h 中 */

/* ========== IDT 门描述符标志 ========== */

#define IDT_FLAG_PRESENT    0x80
#define IDT_FLAG_DPL0       0x00    /* Ring 0 */
#define IDT_FLAG_DPL1       0x20    /* Ring 1 */
#define IDT_FLAG_DPL2       0x40    /* Ring 2 */
#define IDT_FLAG_DPL3       0x60    /* Ring 3 */
#define IDT_FLAG_INTERRUPT  0x0E    /* 64-bit interrupt gate (long mode) */
#define IDT_FLAG_TRAP       0x0F    /* 64-bit trap gate (long mode) */
#define IDT_FLAG_32BIT      IDT_FLAG_INTERRUPT  /* backward compat alias */
#define IDT_FLAG_64BIT      IDT_FLAG_INTERRUPT  /* backward compat alias */

/* IST 索引 */
#define IDT_IST_NONE        0
#define IDT_IST_DF          1       /* 双重错误使用 IST1 */
#define IDT_IST_NMI         2       /* NMI 使用 IST2 */
#define IDT_IST_MC          3       /* 机器检查使用 IST3 */
#define IDT_IST_DB          4       /* 调试使用 IST4 */

/* ========== 页错误标志 ========== */

#define PF_FLAG_PRESENT     (1 << 0)    /* 页存在 */
#define PF_FLAG_WRITE       (1 << 1)    /* 写操作 */
#define PF_FLAG_USER        (1 << 2)    /* 用户态 */
#define PF_FLAG_RSVD        (1 << 3)    /* 保留位 */
#define PF_FLAG_EXEC        (1 << 4)    /* 指令获取 */
#define PF_FLAG_PK          (1 << 5)    /* 保护密钥 */
#define PF_FLAG_SS          (1 << 6)    /* 阴影栈 */

/* ========== 中断栈帧 ========== */

/*
 * 与 idt_asm.S 中的 push 顺序匹配
 * 
 * 栈布局（从高地址到低地址）：
 *   rax, rbx, rcx, rdx, rsi, rdi, rbp, r8-r15 (软件保存)
 *   int_no, err_code (软件压入)
 *   rip, cs, rflags, rsp, ss (CPU 自动压入) <- 栈顶 (rsp 指向这里)
 * 
 * 由于 frame 指针指向栈顶（低地址），结构体成员必须从栈顶开始排列。
 */
struct interrupt_frame {
    /* 栈顶 (rsp 指向这里，低地址) - 最后 push 的寄存器 */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;     /* 软件保存 r8-r15 (r15 最后 push) */
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;         /* 软件保存通用寄存器 (rax 最先 push) */
    uint64_t int_no, err_code;                          /* 软件压入的中断号和错误码 */
    /* CPU 自动压入 (高地址，栈底) */
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

/* ========== 编译时验证结构体布局 ========== */
/* 这些断言确保结构体与汇编代码的栈布局匹配 */
/* 参考 idt_asm.S 中的 push 顺序和 isr_common_stub */
/* 
 * 汇编 push 顺序: int_no, err_code (之前压入), rax, rbx, ..., r15
 * 栈顶 (rsp) = r15, 栈底 = ss
 * 结构体字段从低地址到高地址排列
 */

/* 验证结构体大小: 22 个 uint64_t = 176 字节 */
_Static_assert(sizeof(struct interrupt_frame) == 176,
    "interrupt_frame size mismatch! Expected 176 bytes. Check struct layout vs idt_asm.S");

/* 验证栈顶字段 (低地址，最后 push) */
_Static_assert(offsetof(struct interrupt_frame, r15) == 0,
    "interrupt_frame.r15 offset must be 0 (stack top)");
_Static_assert(offsetof(struct interrupt_frame, r8) == 56,
    "interrupt_frame.r8 offset must be 56");

/* 验证通用寄存器 */
_Static_assert(offsetof(struct interrupt_frame, rbp) == 64,
    "interrupt_frame.rbp offset must be 64");
_Static_assert(offsetof(struct interrupt_frame, rax) == 112,
    "interrupt_frame.rax offset must be 112");

/* 验证软件压入字段 */
_Static_assert(offsetof(struct interrupt_frame, int_no) == 120,
    "interrupt_frame.int_no offset must be 120");
_Static_assert(offsetof(struct interrupt_frame, err_code) == 128,
    "interrupt_frame.err_code offset must be 128");

/* 验证 CPU 自动压入字段 (高地址，栈底) */
_Static_assert(offsetof(struct interrupt_frame, rip) == 136,
    "interrupt_frame.rip offset must be 136");
_Static_assert(offsetof(struct interrupt_frame, cs) == 144,
    "interrupt_frame.cs offset must be 144");
_Static_assert(offsetof(struct interrupt_frame, rflags) == 152,
    "interrupt_frame.rflags offset must be 152");
_Static_assert(offsetof(struct interrupt_frame, rsp) == 160,
    "interrupt_frame.rsp offset must be 160");
_Static_assert(offsetof(struct interrupt_frame, ss) == 168,
    "interrupt_frame.ss offset must be 168 (stack bottom)");

/* ========== IDT 门描述符 ========== */

/* 16字节，64位模式 */
struct idt_entry {
    uint16_t offset_low;        /* 偏移 [15:0] */
    uint16_t selector;          /* 段选择子 */
    uint8_t  ist;               /* IST 索引 */
    uint8_t  type_attr;         /* 类型和属性 */
    uint16_t offset_mid;        /* 偏移 [31:16] */
    uint32_t offset_high;       /* 偏移 [63:32] */
    uint32_t reserved;          /* 保留 */
} __attribute__((packed));

/* IDT 指针结构 */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* ========== 中断处理函数类型 ========== */

typedef void (*isr_handler_t)(struct interrupt_frame *frame);

/* ========== 辅助内联函数 ========== */

/* 检查是否为异常（而非中断） */
static inline int is_exception(uint8_t num) {
    return num < IRQ_BASE;
}

/* 检查是否为硬件中断 */
static inline int is_irq(uint8_t num) {
    return num >= IRQ_BASE && num < IRQ_BASE + 16;
}

/* 获取 IRQ 号 */
static inline int get_irq_num(uint8_t num) {
    if (is_irq(num)) {
        return num - IRQ_BASE;
    }
    return -1;
}

/* 获取异常名称 */
static inline const char *get_exception_name(uint8_t num) {
    switch (num) {
        case INT_DE: return INT_DE_NAME;
        case INT_DB: return INT_DB_NAME;
        case INT_NMI: return INT_NMI_NAME;
        case INT_BP: return INT_BP_NAME;
        case INT_OF: return INT_OF_NAME;
        case INT_BR: return INT_BR_NAME;
        case INT_UD: return INT_UD_NAME;
        case INT_NM: return INT_NM_NAME;
        case INT_DF: return INT_DF_NAME;
        case INT_TS: return INT_TS_NAME;
        case INT_NP: return INT_NP_NAME;
        case INT_SS: return INT_SS_NAME;
        case INT_GP: return INT_GP_NAME;
        case INT_PF: return INT_PF_NAME;
        case INT_MF: return INT_MF_NAME;
        case INT_AC: return INT_AC_NAME;
        case INT_MC: return INT_MC_NAME;
        case INT_XM: return INT_XM_NAME;
        default: return "Unknown Exception";
    }
}

/* 检查中断帧是否来自用户态 */
static inline int frame_from_user(struct interrupt_frame *frame) {
    return (frame->cs & 3) != 0;
}

/* 检查中断帧是否来自内核态 */
static inline int frame_from_kernel(struct interrupt_frame *frame) {
    return (frame->cs & 3) == 0;
}

/* ========== 函数声明 ========== */

/* 初始化 IDT */
void idt_init(void);

/* 设置 IDT 门 */
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t type_attr);

/* 设置 IDT 门 (带 IST) */
void idt_set_gate_ist(uint8_t num, uint64_t handler, uint16_t selector, 
                      uint8_t type_attr, uint8_t ist);

/* 注册中断处理函数 */
void isr_register_handler(uint8_t num, isr_handler_t handler);

/* 注销中断处理函数 */
void isr_unregister_handler(uint8_t num);

/* 加载 IDT (汇编实现) */
extern void idt_load(struct idt_ptr *idt_ptr);

/* 重新加载当前 CPU 的 IDT */
void idt_reload(void);

/* 中断入口 (汇编实现) */
extern void isr_common_stub(void);

/* 打印中断帧信息 */
void dump_interrupt_frame(struct interrupt_frame *frame);

/* 中断统计 */
void idt_stats(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * IST (Interrupt Stack Table) 管理
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 初始化 IST: 分配独立栈并写入 TSS */
void idt_init_ist(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * 中断嵌套优化
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 检查是否需要完整上下文保存 (浅嵌套=是, 深嵌套=否) */
int idt_need_full_context_save(void);

/* 获取嵌套链中的中断向量 (用于调试) */
int idt_get_nested_vector(int depth, uint8_t *vector, uint64_t *rip);

/* ═══════════════════════════════════════════════════════════════════════════
 * IRQ 自动均衡
 * ═══════════════════════════════════════════════════════════════════════════ */

/* IRQ 自动均衡: 根据中断频率在 CPU 间重新分配 IRQ */
void idt_auto_balance_irqs(int cpu_count, uint64_t current_tick);

/* 获取均衡统计 */
void idt_get_balance_stats(uint32_t *balance_count, uint32_t *migrations);

#endif
