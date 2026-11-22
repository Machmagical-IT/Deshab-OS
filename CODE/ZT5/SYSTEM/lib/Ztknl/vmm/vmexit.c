/*
 * Zratro OS - VM-Exit Handler
 * 
 * 处理所有类型的 VM-Exit
 */

#include "vmexit.h"
#include "vmx.h"
#include "console.h"
#include "string.h"
#include "idt.h"
#include "isr.h"

/* ========== VM-Exit 原因定义 ========== */

#define EXIT_REASON_EXCEPTION_NMI       1
#define EXIT_REASON_EXTERNAL_INTERRUPT  2
#define EXIT_REASON_INIT                3
#define EXIT_REASON_SHUTDOWN            4
#define EXIT_REASON_IO_SMI              5
#define EXIT_REASON_SMI                 6
#define EXIT_REASON_PENDING_VIRT_INTR   7
#define EXIT_REASON_NMI_WINDOW          8
#define EXIT_REASON_TASK_SWITCH         9
#define EXIT_REASON_CPUID               10
#define EXIT_REASON_GETSEC              11
#define EXIT_REASON_HLT                 12
#define EXIT_REASON_INVD                13
#define EXIT_REASON_INVLPG              14
#define EXIT_REASON_RDPMC               15
#define EXIT_REASON_RDTSC               16
#define EXIT_REASON_RSM                 17
#define EXIT_REASON_VMCALL              18
#define EXIT_REASON_VMCLEAR             19
#define EXIT_REASON_VMLAUNCH            20
#define EXIT_REASON_VMPTRLD             21
#define EXIT_REASON_VMPTRST             22
#define EXIT_REASON_VMREAD              23
#define EXIT_REASON_VMRESUME            24
#define EXIT_REASON_VMWRITE             25
#define EXIT_REASON_VMXOFF              26
#define EXIT_REASON_VMXON               27
#define EXIT_REASON_CR_ACCESS           28
#define EXIT_REASON_DR_ACCESS           29
#define EXIT_REASON_IO_INSTRUCTION      30
#define EXIT_REASON_MSR_READ            31
#define EXIT_REASON_MSR_WRITE           32
#define EXIT_REASON_INVALID_GUEST_STATE 33
#define EXIT_REASON_MSR_LOADING         34
#define EXIT_REASON_MWAIT_INSTRUCTION   36
#define EXIT_REASON_MONITOR_TRAP_FLAG   37
#define EXIT_REASON_MONITOR_INSTRUCTION 39
#define EXIT_REASON_PAUSE_INSTRUCTION   40
#define EXIT_REASON_MACHINE_CHECK       41
#define EXIT_REASON_TPR_BELOW_THRESHOLD 43
#define EXIT_REASON_APIC_ACCESS         44
#define EXIT_REASON_VIRTUALIZED_EOI     45
#define EXIT_REASON_XDTR_ACCESS         46
#define EXIT_REASON_TR_ACCESS           47
#define EXIT_REASON_EPT_VIOLATION       48
#define EXIT_REASON_EPT_MISCONFIG       49
#define EXIT_REASON_INVEPT              50
#define EXIT_REASON_RDTSCP              51
#define EXIT_REASON_PREEMPTION_TIMER    52
#define EXIT_REASON_INVVPID             53
#define EXIT_REASON_WBINVD              54
#define EXIT_REASON_XSETBV              55
#define EXIT_REASON_APIC_WRITE          56
#define EXIT_REASON_RDRAND              57
#define EXIT_REASON_INVPCID             58
#define EXIT_REASON_VMFUNC              59
#define EXIT_REASON_ENCLS               60
#define EXIT_REASON_RDSEED              61
#define EXIT_REASON_PCOMMIT             62
#define EXIT_REASON_SPP                 63
#define EXIT_REASON_UMWAIT              64
#define EXIT_REASON_TPAUSE              65

/* VM-Exit 中断信息字段 */
#define VMEXIT_INTR_INFO_VEC_MASK       0x000000FF
#define VMEXIT_INTR_INFO_TYPE_MASK      0x00000700
#define VMEXIT_INTR_INFO_TYPE_SHIFT     8
#define VMEXIT_INTR_INFO_ERROR_CODE     0x00000800
#define VMEXIT_INTR_INFO_VALID          0x00008000

/* 中断类型 */
#define INTR_TYPE_EXT_INT               0
#define INTR_TYPE_NMI                   2
#define INTR_TYPE_HARDWARE_EXCEPTION    3
#define INTR_TYPE_SOFT_INT              4
#define INTR_TYPE_PRIV_SOFT_INT         5
#define INTR_TYPE_SOFT_EXCEPTION        6

/* VM-Exit 指令信息长度 */
#define VM_EXIT_INSTRUCTION_LEN         0x440C
#define VM_EXIT_INST_RETIREMENT         0x440E

/* ========== 外部函数声明 ========== */

extern uint64_t vmcs_read(uint32_t field);
extern int vmcs_write(uint32_t field, uint64_t value);
extern void vmx_disable(void);

/* ========== 前向声明 ========== */

static void handle_exception_nmi(uint64_t exit_qualification);
static void handle_cpuid(void);
static void handle_hlt(void);
static void handle_io_instruction(uint64_t exit_qualification);
static void handle_msr_read(void);
static void handle_msr_write(void);
static void handle_ept_violation(uint64_t exit_qualification);
static void handle_invalid_guest_state(void);
static void handle_vmcall(uint64_t exit_qualification);

/* ========== VM-Exit 统计 ========== */

static struct vmexit_stats stats;

/* ========== VM-Exit 主处理函数 ========== */

void vmexit_handler(struct interrupt_frame *frame) {
    (void)frame;
    
    /* 读取 VM-Exit 原因 */
    uint32_t exit_reason = vmcs_read(0x4402) & 0xFFFF;  /* VM_EXIT_REASON */
    uint64_t exit_qualification = vmcs_read(0x4400);    /* EXIT_QUALIFICATION */
    
    stats.total++;
    
    /* 根据原因分发处理 */
    switch (exit_reason) {
        case EXIT_REASON_EXCEPTION_NMI:
            handle_exception_nmi(exit_qualification);
            stats.exceptions++;
            break;
            
        case EXIT_REASON_CPUID:
            handle_cpuid();
            stats.cpuid++;
            break;
            
        case EXIT_REASON_HLT:
            handle_hlt();
            stats.hlt++;
            break;
            
        case EXIT_REASON_IO_INSTRUCTION:
            handle_io_instruction(exit_qualification);
            stats.io++;
            break;
            
        case EXIT_REASON_MSR_READ:
            handle_msr_read();
            stats.msr_read++;
            break;
            
        case EXIT_REASON_MSR_WRITE:
            handle_msr_write();
            stats.msr_write++;
            break;
            
        case EXIT_REASON_EPT_VIOLATION:
            handle_ept_violation(exit_qualification);
            stats.ept_violation++;
            break;
            
        case EXIT_REASON_INVALID_GUEST_STATE:
            handle_invalid_guest_state();
            stats.invalid_state++;
            break;
            
        case EXIT_REASON_VMCALL:
            handle_vmcall(exit_qualification);
            stats.vmcall++;
            break;
            
        default:
            print("[VM-Exit] Unhandled exit reason: 0x");
            char buf[9];
            for (int i = 7; i >= 0; i--) {
                buf[i] = "0123456789ABCDEF"[exit_reason & 0xF];
                exit_reason >>= 4;
            }
            buf[8] = '\0';
            println(buf);
            stats.unhandled++;
            break;
    }
    
    /* VM-Resume */
    asm volatile("vmresume" : : : "memory");
}

/* ========== 异常/NMI 处理 ========== */

static void handle_exception_nmi(uint64_t exit_qualification) {
    /* 读取中断信息 */
    uint32_t intr_info = vmcs_read(0x4404);  /* VM_ENTRY_INTR_INFO */
    uint32_t vector = intr_info & VMEXIT_INTR_INFO_VEC_MASK;
    uint32_t type = (intr_info & VMEXIT_INTR_INFO_TYPE_MASK) >> VMEXIT_INTR_INFO_TYPE_SHIFT;
    uint32_t error_code = 0;
    
    if (intr_info & VMEXIT_INTR_INFO_ERROR_CODE) {
        error_code = vmcs_read(0x4406);  /* IDT_VECTORING_ERROR_CODE */
    }
    
    (void)exit_qualification;
    (void)type;
    (void)error_code;
    
    /* 根据向量号处理 */
    switch (vector) {
        case 0:  /* #DE - Divide Error */
            print("[VM-Exit] #DE Divide Error\n");
            break;
        case 1:  /* #DB - Debug */
            print("[VM-Exit] #DB Debug\n");
            break;
        case 2:  /* #NMI */
            print("[VM-Exit] NMI\n");
            break;
        case 3:  /* #BP - Breakpoint */
            print("[VM-Exit] #BP Breakpoint\n");
            break;
        case 4:  /* #OF - Overflow */
            print("[VM-Exit] #OF Overflow\n");
            break;
        case 5:  /* #BR - BOUND Range Exceeded */
            print("[VM-Exit] #BR BOUND\n");
            break;
        case 6:  /* #UD - Invalid Opcode */
            print("[VM-Exit] #UD Invalid Opcode\n");
            break;
        case 7:  /* #NM - Device Not Available */
            print("[VM-Exit] #NM Device Not Available\n");
            break;
        case 8:  /* #DF - Double Fault */
            print("[VM-Exit] #DF Double Fault\n");
            break;
        case 10: /* #TS - Invalid TSS */
            print("[VM-Exit] #TS Invalid TSS\n");
            break;
        case 11: /* #NP - Segment Not Present */
            print("[VM-Exit] #NP Segment Not Present\n");
            break;
        case 12: /* #SS - Stack Fault */
            print("[VM-Exit] #SS Stack Fault\n");
            break;
        case 13: /* #GP - General Protection */
            print("[VM-Exit] #GP General Protection\n");
            break;
        case 14: /* #PF - Page Fault */
            {
                uint64_t cr2 = vmcs_read(0x6822);  /* GUEST_LINEAR_ADDRESS */
                print("[VM-Exit] #PF Page Fault at 0x");
                char buf[17];
                for (int i = 15; i >= 0; i--) {
                    buf[i] = "0123456789ABCDEF"[cr2 & 0xF];
                    cr2 >>= 4;
                }
                buf[16] = '\0';
                println(buf);
            }
            break;
        case 16: /* #MF - x87 FPU Error */
            print("[VM-Exit] #MF FPU Error\n");
            break;
        case 17: /* #AC - Alignment Check */
            print("[VM-Exit] #AC Alignment Check\n");
            break;
        case 18: /* #MC - Machine Check */
            print("[VM-Exit] #MC Machine Check\n");
            break;
        case 19: /* #XF - SIMD FPU Exception */
            print("[VM-Exit] #XF SIMD Exception\n");
            break;
        default:
            print("[VM-Exit] Unknown exception vector: ");
            print_hex(vector); println("");
            break;
    }
    
    /* 注入回客户机或处理 */
    /* TODO: 根据需要注入异常或模拟 */
}

/* ========== CPUID 处理 ========== */

static void handle_cpuid(void) {
    uint32_t leaf = vmcs_read(0x681E);  /* GUEST_RAX */
    uint32_t subleaf = vmcs_read(0x681A);  /* GUEST_RCX */
    uint32_t eax, ebx, ecx, edx;
    
    /* 执行 CPUID */
    asm volatile("cpuid" 
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(leaf), "c"(subleaf));
    
    /* 根据需要修改结果 */
    switch (leaf) {
        case 1:  /* Feature flags */
            /* 清除 VMX 标志，避免客户机检测到虚拟化 */
            ecx &= ~(1 << 5);  /* VMX */
            break;
        case 0x7:  /* Extended features */
            if (subleaf == 0) {
                /* 可以清除某些特性标志 */
            }
            break;
        case 0x40000000:  /* Hypervisor interface */
            /* 返回 hypervisor 信息 */
            eax = 0x40000001;
            ebx = 'Z';  /* 'Zrat' */
            ecx = 'r';
            edx = 'a';
            break;
        case 0x40000001:  /* Hypervisor interface */
            eax = 't';  /* 'ro' */
            ebx = 'r';
            ecx = 'o';
            edx = ' ';
            break;
    }
    
    /* 写回寄存器 */
    vmcs_write(0x681E, eax);  /* GUEST_RAX */
    vmcs_write(0x681C, ebx);  /* GUEST_RBX */
    vmcs_write(0x681A, ecx);  /* GUEST_RCX */
    vmcs_write(0x6818, edx);  /* GUEST_RDX */
    
    /* 跳过 CPUID 指令 */
    uint32_t instr_len = vmcs_read(VM_EXIT_INSTRUCTION_LEN);
    uint64_t rip = vmcs_read(0x681E);  /* GUEST_RIP */
    vmcs_write(0x681E, rip + instr_len);
}

/* ========== HLT 处理 ========== */

static void handle_hlt(void) {
    /* 在虚拟机中执行 HLT 会导致 VM-Exit */
    /* 可以选择模拟或让 CPU 进入低功耗状态 */
    
    uint32_t instr_len = vmcs_read(VM_EXIT_INSTRUCTION_LEN);
    uint64_t rip = vmcs_read(0x681E);  /* GUEST_RIP */
    vmcs_write(0x681E, rip + instr_len);
    
    /* 可选：让物理 CPU 也进入 HLT */
    /* asm volatile("hlt"); */
}

/* ========== I/O 指令处理 ========== */

static void handle_io_instruction(uint64_t exit_qualification) {
    uint32_t port = (exit_qualification >> 16) & 0xFFFF;
    uint32_t dir = (exit_qualification >> 3) & 1;
    uint32_t string = (exit_qualification >> 4) & 1;
    uint32_t rep = (exit_qualification >> 5) & 1;
    uint32_t operand_size = (exit_qualification & 7) + 1;
    
    (void)string;
    (void)rep;
    
    if (dir == 0) {
        /* OUT instruction */
        uint64_t data = vmcs_read(0x681E);  /* GUEST_RAX */
        
        print("[VM-Exit] OUT port 0x");
        char buf[5];
        for (int i = 3; i >= 0; i--) {
            buf[i] = "0123456789ABCDEF"[port & 0xF];
            port >>= 4;
        }
        buf[4] = '\0';
        print(buf);
        print(" data 0x");
        print_hex(data); println("");
        
        /* TODO: 模拟 I/O 写入 */
        
    } else {
        /* IN instruction */
        uint64_t data = 0;
        
        print("[VM-Exit] IN port 0x");
        char buf[5];
        for (int i = 3; i >= 0; i--) {
            buf[i] = "0123456789ABCDEF"[port & 0xF];
            port >>= 4;
        }
        buf[4] = '\0';
        println(buf);
        
        /* TODO: 模拟 I/O 读取 */
        
        vmcs_write(0x681E, data);  /* GUEST_RAX */
    }
    
    /* 跳过指令 */
    uint32_t instr_len = vmcs_read(VM_EXIT_INSTRUCTION_LEN);
    uint64_t rip = vmcs_read(0x681E);  /* GUEST_RIP */
    vmcs_write(0x681E, rip + instr_len);
}

/* ========== MSR 读取处理 ========== */

static void handle_msr_read(void) {
    uint32_t msr = vmcs_read(0x681E);  /* GUEST_RCX */
    uint32_t eax, edx;
    
    /* 读取 MSR */
    asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(msr));
    
    /* 写回寄存器 */
    vmcs_write(0x681E, ((uint64_t)edx << 32) | eax);  /* GUEST_RAX */
    
    /* 跳过指令 */
    uint32_t instr_len = vmcs_read(VM_EXIT_INSTRUCTION_LEN);
    uint64_t rip = vmcs_read(0x681E);  /* GUEST_RIP */
    vmcs_write(0x681E, rip + instr_len);
}

/* ========== MSR 写入处理 ========== */

static void handle_msr_write(void) {
    uint32_t msr = vmcs_read(0x681E);  /* GUEST_RCX */
    uint64_t data = vmcs_read(0x681E);  /* GUEST_RAX */
    uint32_t eax = data & 0xFFFFFFFF;
    uint32_t edx = (data >> 32) & 0xFFFFFFFF;
    
    print("[VM-Exit] WRMSR msr 0x");
    char buf[9];
    for (int i = 7; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[msr & 0xF];
        msr >>= 4;
    }
    buf[8] = '\0';
    print(buf);
    print(" data 0x");
    print_hex(data); println("");
    
    /* 写入 MSR */
    asm volatile("wrmsr" : : "a"(eax), "d"(edx), "c"(msr));
    
    /* 跳过指令 */
    uint32_t instr_len = vmcs_read(VM_EXIT_INSTRUCTION_LEN);
    uint64_t rip = vmcs_read(0x681E);  /* GUEST_RIP */
    vmcs_write(0x681E, rip + instr_len);
}

/* ========== EPT Violation 处理 ========== */

static void handle_ept_violation(uint64_t exit_qualification) {
    uint64_t gpa = vmcs_read(0x6400);  /* GUEST_PHYSICAL_ADDRESS */
    uint64_t gla = vmcs_read(0x6822);  /* GUEST_LINEAR_ADDRESS */
    
    uint32_t read = (exit_qualification >> 0) & 1;
    uint32_t write = (exit_qualification >> 1) & 1;
    uint32_t execute = (exit_qualification >> 2) & 1;
    
    (void)gla;
    
    print("[VM-Exit] EPT Violation: ");
    if (read) print("R");
    if (write) print("W");
    if (execute) print("X");
    print(" GPA 0x");
    
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[gpa & 0xF];
        gpa >>= 4;
    }
    buf[16] = '\0';
    println(buf);
    
    /* TODO: 动态映射 EPT 页面或注入页错误 */
}

/* ========== 无效客户机状态处理 ========== */

static void handle_invalid_guest_state(void) {
    print("[VM-Exit] Invalid Guest State!\n");
    
    /* 打印调试信息 */
    print("  CR0: 0x");
    print_hex(vmcs_read(0x6800)); println("");
    print("  CR3: 0x");
    print_hex(vmcs_read(0x6802)); println("");
    print("  CR4: 0x");
    print_hex(vmcs_read(0x6804)); println("");
    print("  RIP: 0x");
    print_hex(vmcs_read(0x681E)); println("");
    print("  RSP: 0x");
    print_hex(vmcs_read(0x681C)); println("");
    print("  RFLAGS: 0x");
    print_hex(vmcs_read(0x6820)); println("");
    
    /* 无法恢复，禁用 VMX */
    vmx_disable();
    
    while (1) {
        asm volatile("hlt");
    }
}

/* ========== VMCALL 处理 ========== */

static void handle_vmcall(uint64_t exit_qualification) {
    uint64_t rax = vmcs_read(0x681E);  /* GUEST_RAX */
    uint64_t rbx = vmcs_read(0x681C);  /* GUEST_RBX */
    uint64_t rcx = vmcs_read(0x681A);  /* GUEST_RCX */
    uint64_t rdx = vmcs_read(0x6818);  /* GUEST_RDX */
    
    (void)rbx;
    (void)rcx;
    (void)rdx;
    
    print("[VM-Exit] VMCALL function 0x");
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[rax & 0xF];
        rax >>= 4;
    }
    buf[16] = '\0';
    println(buf);
    
    /* 处理 hypercall */
    switch (exit_qualification) {
        case 0:  /* Shutdown */
            print("[VM-Exit] Guest requested shutdown\n");
            break;
        case 1:  /* Send message */
            print("[VM-Exit] Guest sent message\n");
            break;
        default:
            print("[VM-Exit] Unknown hypercall\n");
            break;
    }
    
    /* 跳过指令 */
    uint32_t instr_len = vmcs_read(VM_EXIT_INSTRUCTION_LEN);
    uint64_t rip = vmcs_read(0x681E);  /* GUEST_RIP */
    vmcs_write(0x681E, rip + instr_len);
}

/* ========== 统计接口 ========== */

void vmexit_get_stats(struct vmexit_stats *s) {
    if (s) {
        *s = stats;
    }
}

void vmexit_print_stats(void) {
    print("[VM-Exit Statistics]\n");
    print("  Total: ");
    print_hex(stats.total); println("");
    print("  Exceptions: ");
    print_hex(stats.exceptions); println("");
    print("  CPUID: ");
    print_hex(stats.cpuid); println("");
    print("  HLT: ");
    print_hex(stats.hlt); println("");
    print("  I/O: ");
    print_hex(stats.io); println("");
    print("  MSR Read: ");
    print_hex(stats.msr_read); println("");
    print("  MSR Write: ");
    print_hex(stats.msr_write); println("");
    print("  EPT Violation: ");
    print_hex(stats.ept_violation); println("");
    print("  Invalid State: ");
    print_hex(stats.invalid_state); println("");
    print("  VMCALL: ");
    print_hex(stats.vmcall); println("");
    print("  Unhandled: ");
    print_hex(stats.unhandled); println("");
}
