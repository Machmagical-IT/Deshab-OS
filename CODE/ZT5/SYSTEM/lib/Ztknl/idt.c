#include "idt.h"
#include "isr.h"
#include "apic.h"
#include "console.h"
#include "ktypes.h"
#include "gdt.h"
#include "heap.h"
#include "task.h"
#include "smp.h"

/* 外部函数声明 */
extern void ioapic_mask_irq(uint8_t irq);
extern void ioapic_unmask_irq(uint8_t irq);

/* 最大嵌套深度 */
#define IDT_MAX_NESTING     16

/* IDT 表 */
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idt_ptr;

/* 中断处理函数表 */
static isr_handler_t isr_handlers[IDT_ENTRIES];

/* 外部中断入口 (汇编定义) */
extern void isr_0(void), isr_1(void), isr_2(void), isr_3(void);
extern void isr_4(void), isr_5(void), isr_6(void), isr_7(void);
extern void isr_8(void), isr_9(void), isr_10(void), isr_11(void);
extern void isr_12(void), isr_13(void), isr_14(void), isr_15(void);
extern void isr_16(void), isr_17(void), isr_18(void), isr_19(void);
extern void isr_20(void), isr_21(void), isr_22(void), isr_23(void);
extern void isr_24(void), isr_25(void), isr_26(void), isr_27(void);
extern void isr_28(void), isr_29(void), isr_30(void), isr_31(void);

/* IRQ 入口 */
extern void irq_0(void), irq_1(void), irq_2(void), irq_3(void);
extern void irq_4(void), irq_5(void), irq_6(void), irq_7(void);
extern void irq_8(void), irq_9(void), irq_10(void), irq_11(void);
extern void irq_12(void), irq_13(void), irq_14(void), irq_15(void);

/* APIC 中断入口 */
extern void apic_timer_isr(void);
extern void apic_error_isr(void);
extern void apic_spurious_isr(void);

/* IPI 中断入口 */
extern void ipi_reschedule_isr(void);
extern void ipi_call_function_isr(void);
extern void ipi_cpu_stop_isr(void);
extern void ipi_cpu_start_isr(void);
extern void ipi_tlb_shootdown_isr(void);
extern void ipi_mem_barrier_isr(void);

/* 系统调用入口 */
extern void syscall_isr(void);

void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t type_attr) {
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].selector    = selector;
    idt[num].ist         = 0;
    idt[num].type_attr   = type_attr;
    idt[num].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].reserved    = 0;
}

/* 设置 IDT 门 (带 IST 索引) */
void idt_set_gate_ist(uint8_t num, uint64_t handler, uint16_t selector,
                      uint8_t type_attr, uint8_t ist) {
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].selector    = selector;
    idt[num].ist         = ist & 0x07;  /* IST 只有 3 位 (0-7) */
    idt[num].type_attr   = type_attr;
    idt[num].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].reserved    = 0;
}

void isr_register_handler(uint8_t num, isr_handler_t handler) {
    isr_handlers[num] = handler;
}

/* 通用中断处理程序 (从汇编调用) */
void isr_handler(struct interrupt_frame *frame) {
    /* 追踪所有中断 */
    if (frame->int_no < 32) {
        serial_print("[ISR] int_no=");
        char buf[8]; int p = 0; uint64_t v = frame->int_no;
        if (v == 0) buf[p++] = '0';
        else { char t[8]; int tp = 0; while (v > 0) { t[tp++] = '0' + v % 10; v /= 10; } while (tp > 0) buf[p++] = t[--tp]; }
        buf[p] = '\0';
        serial_print(buf);
        serial_print(" RIP=0x");
        char rbuf[20]; int rp = 0; uint64_t rv = frame->rip;
        for (int k = 15; k >= 0; k--) { rbuf[rp++] = "0123456789ABCDEF"[(rv >> (k*4)) & 0xF]; }
        rbuf[16] = '\0';
        serial_print(rbuf);
        
        /* 对于页错误，打印 CR2 和 err_code */
        if (frame->int_no == 14) {
            uint64_t cr2;
            asm volatile("mov %%cr2, %0" : "=r"(cr2));
            serial_print(" CR2=0x");
            char cbuf[20]; int cp = 0;
            for (int k = 15; k >= 0; k--) { cbuf[cp++] = "0123456789ABCDEF"[(cr2 >> (k*4)) & 0xF]; }
            cbuf[16] = '\0';
            serial_print(cbuf);
            serial_print(" err=0x");
            uint64_t ev = frame->err_code;
            for (int k = 1; k >= 0; k--) { cbuf[1-k] = "0123456789ABCDEF"[(ev >> (k*4)) & 0xF]; }
            cbuf[2] = '\0';
            serial_print(cbuf);
        }
        serial_print("\n");
    }
    
    /* 边界检查：确保中断号在有效范围内 */
    if (frame->int_no >= IDT_ENTRIES) {
        println("");
        println("=== ISR FATAL ERROR ===");
        print("frame pointer: 0x");
        print_hex((uint64_t)frame);
        println("");
        print("frame->int_no: 0x");
        print_hex(frame->int_no);
        println("");
        print("frame->err_code: 0x");
        print_hex(frame->err_code);
        println("");
        print("frame->rip: 0x");
        print_hex(frame->rip);
        println("");
        print("frame->cs: 0x");
        print_hex(frame->cs);
        println("");
        print("frame->rflags: 0x");
        print_hex(frame->rflags);
        println("");
        /* 打印栈上原始数据来验证结构体布局 */
        println("Stack dump at frame:");
        uint64_t *stack = (uint64_t *)frame;
        for (int i = 0; i < 12; i++) {
            print("  [");
            print_dec(i);
            print("] offset ");
            print_dec(i * 8);
            print(": 0x");
            print_hex(stack[i]);
            println("");
        }
        println("Halting system...");
        for (;;) { asm volatile("hlt"); }
    }
    
    if (isr_handlers[frame->int_no] != NULL) {
        isr_handlers[frame->int_no](frame);
    } else {
        println("[ISR] Unhandled interrupt: ");
        print_hex(frame->int_no);
        println("");
        for (;;) { asm volatile("hlt"); }
    }
    
    /* 对于硬件中断发送 EOI */
    /* IRQ 0-15 映射到向量 32-47 */
    /* APIC 中断也需要 EOI */
    if (frame->int_no >= 32) {
        apic_eoi();
    }

    /* 注意: 不在此处检查 NEED_RESCHED
     * scheduler_tick() 已经在定时器中断处理中检查 NEED_RESCHED 并调用 schedule()
     * 如果此处也检查，会导致 schedule() 被调用两次，造成无限调度循环 */
}

void idt_init(void) {
    /* 清空 IDT */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0, 0, 0);
        isr_handlers[i] = NULL;
    }
    
    /* 设置 CPU 异常处理 (向量 0-31) */
    /* 注意：关键异常处理器的 DPL 必须设为 3，以便用户态程序触发异常时能正确处理 */
    /* 关键异常使用 IST: NMI=IST2, 双重故障=IST1, 机器检查=IST3, 调试=IST4 */
    idt_set_gate(0,  (uint64_t)isr_0,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 除零 */
    idt_set_gate_ist(1,  (uint64_t)isr_1,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT, IDT_IST_DB);   /* 调试 - IST4 */
    idt_set_gate_ist(2,  (uint64_t)isr_2,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT, IDT_IST_NMI);   /* NMI - IST2 */
    idt_set_gate(3,  (uint64_t)isr_3,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_TRAP);    /* 断点 */
    idt_set_gate(4,  (uint64_t)isr_4,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 溢出 */
    idt_set_gate(5,  (uint64_t)isr_5,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 边界 */
    idt_set_gate(6,  (uint64_t)isr_6,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 无效操作码 */
    idt_set_gate(7,  (uint64_t)isr_7,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 设备不可用 */
    idt_set_gate_ist(8,  (uint64_t)isr_8,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT, IDT_IST_DF);   /* 双重故障 - IST1 */
    idt_set_gate(9,  (uint64_t)isr_9,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 协处理器段超越 */
    idt_set_gate(10, (uint64_t)isr_10, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 无效 TSS */
    idt_set_gate(11, (uint64_t)isr_11, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 段不存在 */
    idt_set_gate(12, (uint64_t)isr_12, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 栈段错误 */
    idt_set_gate(13, (uint64_t)isr_13, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 常规保护 */
    idt_set_gate(14, (uint64_t)isr_14, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 页错误 */
    idt_set_gate(15, (uint64_t)isr_15, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);                   /* 保留 */
    idt_set_gate(16, (uint64_t)isr_16, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* FPU 错误 */
    idt_set_gate(17, (uint64_t)isr_17, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* 对齐检查 */
    idt_set_gate_ist(18, (uint64_t)isr_18, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT, IDT_IST_MC);   /* 机器检查 - IST3 */
    idt_set_gate(19, (uint64_t)isr_19, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_32BIT);   /* SIMD 异常 */
    idt_set_gate(20, (uint64_t)isr_20, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);                   /* 保留 */
    
    /* 保留异常 21-31 */
    for (int i = 21; i < 32; i++) {
        idt_set_gate(i, (uint64_t)isr_0, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    }
    
    /* 注册异常处理程序 */
    extern void isr_handler_divide_error(struct interrupt_frame *);
    extern void isr_handler_debug(struct interrupt_frame *);
    extern void isr_handler_nmi(struct interrupt_frame *);
    extern void isr_handler_breakpoint(struct interrupt_frame *);
    extern void isr_handler_overflow(struct interrupt_frame *);
    extern void isr_handler_bound_range(struct interrupt_frame *);
    extern void isr_handler_invalid_opcode(struct interrupt_frame *);
    extern void isr_handler_device_not_available(struct interrupt_frame *);
    extern void isr_handler_double_fault(struct interrupt_frame *);
    extern void isr_handler_invalid_tss(struct interrupt_frame *);
    extern void isr_handler_segment_not_present(struct interrupt_frame *);
    extern void isr_handler_stack_segment_fault(struct interrupt_frame *);
    extern void isr_handler_general_protection(struct interrupt_frame *);
    extern void isr_handler_page_fault(struct interrupt_frame *);
    extern void isr_handler_fpu_error(struct interrupt_frame *);
    extern void isr_handler_alignment_check(struct interrupt_frame *);
    extern void isr_handler_machine_check(struct interrupt_frame *);
    extern void isr_handler_simd_exception(struct interrupt_frame *);
    
    isr_handlers[0]  = isr_handler_divide_error;
    isr_handlers[1]  = isr_handler_debug;
    isr_handlers[2]  = isr_handler_nmi;
    isr_handlers[3]  = isr_handler_breakpoint;
    isr_handlers[4]  = isr_handler_overflow;
    isr_handlers[5]  = isr_handler_bound_range;
    isr_handlers[6]  = isr_handler_invalid_opcode;
    isr_handlers[7]  = isr_handler_device_not_available;
    isr_handlers[8]  = isr_handler_double_fault;
    isr_handlers[10] = isr_handler_invalid_tss;
    isr_handlers[11] = isr_handler_segment_not_present;
    isr_handlers[12] = isr_handler_stack_segment_fault;
    isr_handlers[13] = isr_handler_general_protection;
    isr_handlers[14] = isr_handler_page_fault;
    isr_handlers[16] = isr_handler_fpu_error;
    isr_handlers[17] = isr_handler_alignment_check;
    isr_handlers[18] = isr_handler_machine_check;
    isr_handlers[19] = isr_handler_simd_exception;
    
    /* 设置 IRQ 处理 (向量 32-47) */
    idt_set_gate(32, (uint64_t)irq_0,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(33, (uint64_t)irq_1,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(34, (uint64_t)irq_2,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(35, (uint64_t)irq_3,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(36, (uint64_t)irq_4,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(37, (uint64_t)irq_5,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(38, (uint64_t)irq_6,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(39, (uint64_t)irq_7,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(40, (uint64_t)irq_8,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(41, (uint64_t)irq_9,  0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(42, (uint64_t)irq_10, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(43, (uint64_t)irq_11, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(44, (uint64_t)irq_12, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(45, (uint64_t)irq_13, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(46, (uint64_t)irq_14, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(47, (uint64_t)irq_15, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    
    /* APIC 中断 (使用 apic.h 中定义的向量) */
    idt_set_gate(APIC_TIMER_VECTOR,   (uint64_t)apic_timer_isr,   0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(APIC_ERROR_VECTOR,   (uint64_t)apic_error_isr,   0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(APIC_SPURIOUS_VECTOR, (uint64_t)apic_spurious_isr, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    
    /* IPI 中断 (使用 smp.h 中定义的向量) */
    idt_set_gate(0x60, (uint64_t)ipi_reschedule_isr,    0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(0x61, (uint64_t)ipi_call_function_isr, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(0x62, (uint64_t)ipi_cpu_stop_isr,      0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(0x63, (uint64_t)ipi_cpu_start_isr,     0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(0x64, (uint64_t)ipi_tlb_shootdown_isr, 0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    idt_set_gate(0x65, (uint64_t)ipi_mem_barrier_isr,   0x08, IDT_FLAG_PRESENT | IDT_FLAG_32BIT);
    
    /* 系统调用 (用户可调用) */
    idt_set_gate(SYSCALL_VECTOR, (uint64_t)syscall_isr, 0x08, 
                 IDT_FLAG_PRESENT | IDT_FLAG_DPL3 | IDT_FLAG_TRAP);
    
    /* 加载 IDT */
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)&idt;
    idt_load(&idt_ptr);
}

/* 重新加载 IDT (用于 AP 启动) */
void idt_reload(void) {
    extern struct idt_ptr idt_ptr;
    idt_load(&idt_ptr);
}

/* 注销中断处理函数 */
void isr_unregister_handler(uint8_t num) {
    if (num < IDT_ENTRIES) {
        isr_handlers[num] = NULL;
    }
}

/* 转储中断帧信息 */
void dump_interrupt_frame(struct interrupt_frame *frame) {
    if (frame == NULL) {
        println("[DUMP] NULL frame pointer");
        return;
    }
    
    println("=== Interrupt Frame Dump ===");
    
    print("  Vector: 0x");
    print_hex(frame->int_no);
    print(" (");
    print(get_exception_name(frame->int_no));
    println(")");
    
    print("  Error Code: 0x");
    print_hex(frame->err_code);
    println("");
    
    println("  --- Registers ---");
    print("  RIP: 0x");
    print_hex(frame->rip);
    print("  CS:  0x");
    print_hex(frame->cs);
    println("");
    
    print("  RSP: 0x");
    print_hex(frame->rsp);
    print("  SS:  0x");
    print_hex(frame->ss);
    println("");
    
    print("  RFLAGS: 0x");
    print_hex(frame->rflags);
    println("");
    
    println("  --- General Purpose ---");
    print("  RAX: 0x");
    print_hex(frame->rax);
    print("  RBX: 0x");
    print_hex(frame->rbx);
    println("");
    
    print("  RCX: 0x");
    print_hex(frame->rcx);
    print("  RDX: 0x");
    print_hex(frame->rdx);
    println("");
    
    print("  RSI: 0x");
    print_hex(frame->rsi);
    print("  RDI: 0x");
    print_hex(frame->rdi);
    println("");
    
    print("  RBP: 0x");
    print_hex(frame->rbp);
    println("");
    
    println("  --- Extended ---");
    print("  R8:  0x");
    print_hex(frame->r8);
    print("  R9:  0x");
    print_hex(frame->r9);
    println("");
    
    print("  R10: 0x");
    print_hex(frame->r10);
    print("  R11: 0x");
    print_hex(frame->r11);
    println("");
    
    print("  R12: 0x");
    print_hex(frame->r12);
    print("  R13: 0x");
    print_hex(frame->r13);
    println("");
    
    print("  R14: 0x");
    print_hex(frame->r14);
    print("  R15: 0x");
    print_hex(frame->r15);
    println("");
}

/* IDT 统计信息 */
void idt_stats(void) {
    int handler_count = 0;
    
    println("=== IDT Statistics ===");
    
    for (int i = 0; i < IDT_ENTRIES; i++) {
        if (isr_handlers[i] != NULL) {
            handler_count++;
        }
    }
    
    print("  Total entries: ");
    print_dec(IDT_ENTRIES);
    println("");
    
    print("  Registered handlers: ");
    print_dec(handler_count);
    println("");
    
    print("  IDT base: 0x");
    print_hex((uint64_t)&idt);
    println("");
    
    print("  IDT limit: ");
    print_dec(idt_ptr.limit);
    println(" bytes");
}

/* ========== IDT 高级增强功能 ========== */

/* === 中断优先级管理 === */

/* 中断优先级级别 */
#define IDT_PRIORITY_MAX       15
#define IDT_PRIORITY_HIGH      12
#define IDT_PRIORITY_NORMAL    8
#define IDT_PRIORITY_LOW       4
#define IDT_PRIORITY_MIN       0

/* 优先级映射表 */
static uint8_t idt_priority[IDT_ENTRIES];

/* 设置中断优先级 */
void idt_set_priority(uint8_t vector, uint8_t priority) {
    if (vector < IDT_ENTRIES) {
        idt_priority[vector] = priority & 0x0F;
    }
}

/* 获取中断优先级 */
uint8_t idt_get_priority(uint8_t vector) {
    if (vector < IDT_ENTRIES) {
        return idt_priority[vector];
    }
    return IDT_PRIORITY_NORMAL;
}

/* === 中断嵌套控制 === */

/* 嵌套深度 */
static volatile int idt_nesting_depth = 0;
static uint64_t idt_interrupted_rip[IDT_MAX_NESTING];
static uint8_t idt_interrupted_vector[IDT_MAX_NESTING];

/* 进入中断 */
void idt_enter_interrupt(uint8_t vector, uint64_t rip) {
    if (idt_nesting_depth < IDT_MAX_NESTING) {
        idt_interrupted_rip[idt_nesting_depth] = rip;
        idt_interrupted_vector[idt_nesting_depth] = vector;
        idt_nesting_depth++;
    }
}

/* 退出中断 */
void idt_exit_interrupt(void) {
    if (idt_nesting_depth > 0) {
        idt_nesting_depth--;
    }
}

/* 获取嵌套深度 */
int idt_get_nesting_depth(void) {
    return idt_nesting_depth;
}

/* === 中断统计 === */

struct idt_interrupt_stats {
    uint64_t count[IDT_ENTRIES];
    uint64_t total_cycles[IDT_ENTRIES];
    uint64_t max_cycles[IDT_ENTRIES];
    uint64_t last_timestamp[IDT_ENTRIES];
};

static struct idt_interrupt_stats int_stats;

/* 记录中断开始 */
uint64_t idt_record_interrupt_start(uint8_t vector) {
    int_stats.count[vector]++;
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    int_stats.last_timestamp[vector] = tsc;
    return tsc;
}

/* 记录中断结束 */
void idt_record_interrupt_end(uint8_t vector, uint64_t start_tsc) {
    uint64_t end_tsc;
    asm volatile("rdtsc" : "=A"(end_tsc));
    
    uint64_t cycles = end_tsc - start_tsc;
    int_stats.total_cycles[vector] += cycles;
    
    if (cycles > int_stats.max_cycles[vector]) {
        int_stats.max_cycles[vector] = cycles;
    }
}

/* 获取中断统计 */
void idt_get_interrupt_stats(uint8_t vector, uint64_t *count, 
                             uint64_t *total_cycles, uint64_t *max_cycles) {
    if (vector < IDT_ENTRIES) {
        if (count) *count = int_stats.count[vector];
        if (total_cycles) *total_cycles = int_stats.total_cycles[vector];
        if (max_cycles) *max_cycles = int_stats.max_cycles[vector];
    }
}

/* === 中断屏蔽管理 === */

/* IRQ 屏蔽位图 */
static uint64_t idt_irq_mask = 0;

/* 屏蔽 IRQ */
void idt_mask_irq(uint8_t irq) {
    if (irq < 64) {
        idt_irq_mask |= (1ULL << irq);
        /* 同时更新硬件 */
        if (irq < 16) {
            ioapic_mask_irq(irq);
        }
    }
}

/* 取消屏蔽 IRQ */
void idt_unmask_irq(uint8_t irq) {
    if (irq < 64) {
        idt_irq_mask &= ~(1ULL << irq);
        if (irq < 16) {
            ioapic_unmask_irq(irq);
        }
    }
}

/* 检查 IRQ 是否被屏蔽 */
int idt_is_irq_masked(uint8_t irq) {
    if (irq < 64) {
        return (idt_irq_mask & (1ULL << irq)) != 0;
    }
    return 1;
}

/* === 中断节流 === */

/* 节流配置 */
struct idt_throttle {
    uint32_t max_rate;      /* 每秒最大中断数 */
    uint64_t window_start;  /* 时间窗口起始 */
    uint32_t count;         /* 当前窗口计数 */
    int enabled;
};

static struct idt_throttle throttle_config[IDT_ENTRIES];

/* 设置中断节流 */
void idt_set_throttle(uint8_t vector, uint32_t max_rate) {
    if (vector < IDT_ENTRIES) {
        throttle_config[vector].max_rate = max_rate;
        throttle_config[vector].enabled = (max_rate > 0);
        throttle_config[vector].count = 0;
        throttle_config[vector].window_start = get_tick_count();
    }
}

/* 检查是否应该处理中断 (节流检查) */
int idt_should_handle(uint8_t vector) {
    if (vector >= IDT_ENTRIES) return 1;
    
    struct idt_throttle *t = &throttle_config[vector];
    if (!t->enabled) return 1;
    
    uint64_t now = get_tick_count();
    
    /* 检查是否需要重置窗口 */
    if (now - t->window_start >= 1000) {
        t->window_start = now;
        t->count = 0;
    }
    
    /* 检查是否超过限制 */
    if (t->count >= t->max_rate) {
        return 0;  /* 丢弃中断 */
    }
    
    t->count++;
    return 1;
}

/* === 异常处理增强 === */

/* 异常上下文 */
struct idt_exception_ctx {
    int vector;
    int code;
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t cr2;
    uint64_t cr3;
    int handled;
    int action;  /* 0=继续, 1=终止, 2=调试 */
};

/* 异常处理器 (简化版) */
#define IDT_MAX_EXCEPTION_HANDLERS    4

static int (*exception_handlers[32][IDT_MAX_EXCEPTION_HANDLERS])(struct idt_exception_ctx *);
static int exception_handler_count[32];

/* 注册异常处理器 */
int idt_register_exception_handler_impl(uint8_t vector, 
                                   int (*fn)(struct idt_exception_ctx *)) {
    if (vector >= 32) return -1;
    if (exception_handler_count[vector] >= IDT_MAX_EXCEPTION_HANDLERS) return -1;
    
    exception_handlers[vector][exception_handler_count[vector]++] = fn;
    return 0;
}

/* === 中断调试 === */

/* 中断追踪 */
#define IDT_TRACE_SIZE    256

struct idt_trace_entry {
    uint64_t timestamp;
    uint8_t vector;
    uint8_t type;       /* 0=entry, 1=exit */
    uint64_t rip;
};

static struct idt_trace_entry trace_buffer[IDT_TRACE_SIZE];
static uint32_t trace_index = 0;
static int trace_enabled = 0;

/* 启用追踪 */
void idt_enable_trace(int enable) {
    trace_enabled = enable;
    if (enable) {
        trace_index = 0;
    }
}

/* 记录追踪 */
void idt_trace(uint8_t vector, uint8_t type, uint64_t rip) {
    if (!trace_enabled) return;
    
    uint32_t idx = trace_index % IDT_TRACE_SIZE;
    trace_buffer[idx].timestamp = get_tick_count();
    trace_buffer[idx].vector = vector;
    trace_buffer[idx].type = type;
    trace_buffer[idx].rip = rip;
    
    trace_index++;
}

/* 打印追踪记录 */
void idt_print_trace(void) {
    println("=== IDT Trace Buffer ===");
    
    uint32_t count = trace_index < IDT_TRACE_SIZE ? trace_index : IDT_TRACE_SIZE;
    uint32_t start = trace_index < IDT_TRACE_SIZE ? 0 : trace_index % IDT_TRACE_SIZE;
    
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start + i) % IDT_TRACE_SIZE;
        struct idt_trace_entry *e = &trace_buffer[idx];
        
        print("[");
        print_dec(e->timestamp % 10000000);
        print("] ");
        print(e->type == 0 ? "ENTER" : "EXIT");
        print(" Vector=");
        print_dec(e->vector);
        print(" RIP=0x");
        print_hex(e->rip);
        println("");
    }
}

/* === 诊断报告 === */

void idt_print_report(void) {
    println("╔══════════════════════════════════════════╗");
    println("║         IDT Diagnostic Report            ║");
    println("╚══════════════════════════════════════════╝");
    
    println("");
    println("=== 基本信息 ===");
    print("IDT 条目数: "); print_dec(IDT_ENTRIES); println("");
    
    int handlers = 0;
    for (int i = 0; i < IDT_ENTRIES; i++) {
        if (isr_handlers[i] != NULL) handlers++;
    }
    print("已注册处理程序: "); print_dec(handlers); println("");
    print("当前嵌套深度: "); print_dec(idt_nesting_depth); println("");
    
    println("");
    println("=== 高频中断统计 ===");
    for (int i = 0; i < IDT_ENTRIES; i++) {
        if (int_stats.count[i] > 100) {
            print("Vector "); print_dec(i); print(": ");
            print_dec(int_stats.count[i]); print(" 次, ");
            print("最大 "); print_dec(int_stats.max_cycles[i]); println(" 周期");
        }
    }
    
    println("");
    println("=== 屏蔽的 IRQ ===");
    print("屏蔽位图: "); print_hex(idt_irq_mask); println("");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * IST (Interrupt Stack Table) 管理
 * 为关键中断处理程序提供独立的栈，防止栈溢出导致系统崩溃
 * ═══════════════════════════════════════════════════════════════════════════ */

/* IST 栈大小 (32KB 每个) */
#define IST_STACK_SIZE      0x8000

/* IST 栈空间 (静态分配) */
static uint8_t ist_stack_df[IST_STACK_SIZE] __attribute__((aligned(16)));   /* IST1: 双重故障 */
static uint8_t ist_stack_nmi[IST_STACK_SIZE] __attribute__((aligned(16)));  /* IST2: NMI */
static uint8_t ist_stack_mc[IST_STACK_SIZE] __attribute__((aligned(16)));   /* IST3: 机器检查 */
static uint8_t ist_stack_db[IST_STACK_SIZE] __attribute__((aligned(16)));   /* IST4: 调试 */

/* IST 初始化状态 */
static int ist_initialized = 0;

/* 初始化 IST: 将栈地址写入 TSS 的 IST 字段 */
void idt_init_ist(void) {
    if (ist_initialized) return;

    /* 计算 IST 栈顶地址 (栈向下增长，所以用数组末尾) */
    uint64_t ist1_top = (uint64_t)&ist_stack_df[IST_STACK_SIZE];
    uint64_t ist2_top = (uint64_t)&ist_stack_nmi[IST_STACK_SIZE];
    uint64_t ist3_top = (uint64_t)&ist_stack_mc[IST_STACK_SIZE];
    uint64_t ist4_top = (uint64_t)&ist_stack_db[IST_STACK_SIZE];

    /* 写入 TSS 的 IST 字段 */
    struct tss *tss = gdt_get_per_cpu_tss(0);
    if (tss) {
        tss->ist[0] = ist1_top;  /* IST1: 双重故障 */
        tss->ist[1] = ist2_top;  /* IST2: NMI */
        tss->ist[2] = ist3_top;  /* IST3: 机器检查 */
        tss->ist[3] = ist4_top;  /* IST4: 调试 */
    }

    ist_initialized = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 中断嵌套优化
 * 跟踪嵌套深度，优化上下文保存策略
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 嵌套优化: 浅嵌套 (depth <= 2) 时保存完整上下文
 * 深嵌套 (depth > 2) 时只保存关键寄存器以减少开销 */
#define IDT_SHALLOW_NESTING_THRESHOLD  2

/* 检查是否需要完整上下文保存 */
int idt_need_full_context_save(void) {
    return idt_nesting_depth <= IDT_SHALLOW_NESTING_THRESHOLD;
}

/* 获取嵌套链中的中断向量 (用于调试) */
int idt_get_nested_vector(int depth, uint8_t *vector, uint64_t *rip) {
    if (depth < 0 || depth >= idt_nesting_depth) return -1;
    if (vector) *vector = idt_interrupted_vector[depth];
    if (rip) *rip = idt_interrupted_rip[depth];
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * IRQ 自动均衡
 * 周期性地在 CPU 之间重新分配 IRQ，平衡中断负载
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 均衡配置 */
#define IRQ_BALANCE_INTERVAL_MS    1000    /* 均衡间隔 (毫秒) */
#define IRQ_BALANCE_THRESHOLD      1000    /* 触发均衡的最小中断计数 */
#define IRQ_BALANCE_MAX_ENTRIES    256     /* 最大均衡条目数 */

/* IRQ 均衡条目 */
struct irq_balance {
    uint8_t irq;           /* IRQ 号 */
    uint8_t current_cpu;   /* 当前 CPU */
    uint8_t target_cpu;    /* 目标 CPU */
    uint64_t count;        /* 中断计数 */
};

/* 均衡统计 */
struct irq_balance_stats {
    uint64_t last_balance_time;    /* 上次均衡时间 */
    uint32_t balance_count;        /* 均衡次数 */
    uint32_t migrations;           /* 迁移次数 */
};

static struct irq_balance irq_balance_table[IRQ_BALANCE_MAX_ENTRIES];
static int irq_balance_count = 0;
static struct irq_balance_stats balance_stats = {0, 0, 0};

/* IRQ 自动均衡: 根据中断频率在 CPU 间重新分配 IRQ
 *
 * @param cpu_count 系统 CPU 数量
 * @param current_tick 当前 tick
 */
void idt_auto_balance_irqs(int cpu_count, uint64_t current_tick) {
    if (cpu_count <= 1) return;

    /* 检查均衡间隔 */
    if (current_tick - balance_stats.last_balance_time < IRQ_BALANCE_INTERVAL_MS) {
        return;
    }

    balance_stats.last_balance_time = current_tick;
    balance_stats.balance_count++;

    /* 收集每个 CPU 的中断负载 */
    uint64_t cpu_load[256] = {0};  /* 假设最多 256 个 CPU */
    if (cpu_count > 256) cpu_count = 256;

    /* 统计每个 IRQ 的中断计数和当前 CPU */
    for (int i = 0; i < irq_balance_count; i++) {
        struct irq_balance *ib = &irq_balance_table[i];
        cpu_load[ib->current_cpu] += ib->count;
    }

    /* 计算平均负载 */
    uint64_t total_load = 0;
    for (int c = 0; c < cpu_count; c++) {
        total_load += cpu_load[c];
    }
    uint64_t avg_load = total_load / cpu_count;

    /* 迁移高负载 CPU 上的 IRQ 到低负载 CPU */
    for (int i = 0; i < irq_balance_count; i++) {
        struct irq_balance *ib = &irq_balance_table[i];

        /* 只迁移超过阈值的 IRQ */
        if (ib->count < IRQ_BALANCE_THRESHOLD) {
            ib->count = 0;  /* 重置计数 */
            continue;
        }

        /* 如果当前 CPU 负载高于平均值，迁移到最空闲的 CPU */
        if (cpu_load[ib->current_cpu] > avg_load * 3 / 2) {
            /* 找到最空闲的 CPU */
            int min_cpu = 0;
            uint64_t min_load = cpu_load[0];
            for (int c = 1; c < cpu_count; c++) {
                if (cpu_load[c] < min_load) {
                    min_load = cpu_load[c];
                    min_cpu = c;
                }
            }

            /* 执行迁移 */
            if (min_cpu != ib->current_cpu) {
                /* 更新 I/O APIC 路由 */
                ioapic_set_irq(ib->irq, IRQ_BASE + ib->irq, (uint32_t)min_cpu);

                /* 更新负载统计 */
                cpu_load[ib->current_cpu] -= ib->count;
                cpu_load[min_cpu] += ib->count;

                ib->current_cpu = (uint8_t)min_cpu;
                ib->target_cpu = (uint8_t)min_cpu;
                balance_stats.migrations++;
            }
        }

        /* 重置计数 */
        ib->count = 0;
    }
}

/* 获取均衡统计 */
void idt_get_balance_stats(uint32_t *balance_count, uint32_t *migrations) {
    if (balance_count) *balance_count = balance_stats.balance_count;
    if (migrations) *migrations = balance_stats.migrations;
}
