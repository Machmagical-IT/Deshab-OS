/*
 * Zratro OS - Simplified Page Fault Handling and Prevention Implementation
 */

#include "page_fault.h"
#include "console.h"
#include "string.h"
#include "task.h"
#include "vmm.h"
#include "heap.h"
#include "cpu.h"
#include "smp.h"
#include "pmm.h"
#include "mmap.h"
#include "cow.h"

/* 页错误统计 */
static struct page_fault_stats g_pf_stats = {0};
static struct page_fault_prevention g_pf_prevention = {0};
static spinlock_t g_pf_stats_lock = SPINLOCK_INIT;

/* 按类型的详细统计 */
static uint64_t pf_type_counts[16] = {0};  /* 对应 pf_type_t 枚举值 */
static uint64_t pf_type_handled[16] = {0}; /* 每种类型的已处理计数 */

/* SMP 竞态保护：页面级锁哈希表 */
#define PF_LOCK_HASH_SIZE 256
static spinlock_t pf_page_locks[PF_LOCK_HASH_SIZE];
static volatile int pf_locks_initialized = 0;
static spinlock_t pf_init_lock = SPINLOCK_INIT;

/* 获取页面级锁 */
static spinlock_t *get_pf_page_lock(uint64_t addr) {
    /* 使用双重检查锁定模式 */
    if (!pf_locks_initialized) {
        spin_lock(&pf_init_lock);
        if (!pf_locks_initialized) {
            /* 初始化锁表 */
            for (int i = 0; i < PF_LOCK_HASH_SIZE; i++) {
                pf_page_locks[i] = SPINLOCK_INIT;
            }
            /* 确保初始化完成后再设置标志 */
            asm volatile("" ::: "memory");  /* 内存屏障 */
            pf_locks_initialized = 1;
        }
        spin_unlock(&pf_init_lock);
    }
    
    /* 使用页面地址作为哈希键 */
    uint32_t hash = (uint32_t)((addr >> 12) ^ (addr >> 24) ^ (addr >> 36)) % PF_LOCK_HASH_SIZE;
    return &pf_page_locks[hash];
}

/* 初始化页错误统计 */
void pf_stats_init(void) {
    memset(&g_pf_stats, 0, sizeof(g_pf_stats));
}

/* 记录页错误 */
void pf_stats_record_fault(pf_type_t type) {
    spin_lock(&g_pf_stats_lock);
    g_pf_stats.total_faults++;
    g_pf_stats.unhandled_faults++;  /* 默认为未处理，处理成功后会更新 */
    
    switch (type) {
        case PF_TYPE_NOT_PRESENT:
            g_pf_stats.demand_paging_faults++;
            break;
        case PF_TYPE_PROTECTION:
            g_pf_stats.invalid_access_faults++;
            break;
        case PF_TYPE_STACK_OVERFLOW:
            g_pf_stats.stack_faults++;
            break;
        case PF_TYPE_USER_ACCESS:
            g_pf_stats.user_faults++;
            break;
        case PF_TYPE_DOUBLE_FAULT:
            g_pf_stats.kernel_faults++;  /* 双重错误很严重 */
            break;
        default:
            break;
    }
    spin_unlock(&g_pf_stats_lock);
}

/* 获取页错误统计 */
struct page_fault_stats *pf_get_stats(void) {
    return &g_pf_stats;
}

/* 页错误预防初始化 */
void pf_prevention_init(void) {
    g_pf_prevention.enable_bounds_checking = 1;
    g_pf_prevention.enable_stack_protection = 1;
    g_pf_prevention.enable_kernel_guard = 1;
    g_pf_prevention.enable_user_isolation = 1;
    g_pf_prevention.max_stack_growth = 8 * 1024 * 1024;  /* 8MB */
    g_pf_prevention.guard_page_size = 0x1000;  /* 4KB */
    g_pf_prevention.enable_access_monitoring = 1;
    
    pf_stats_init();
    // print("[PF] Page fault prevention initialized\n");
}

/* 分类页错误 */
pf_type_t pf_classify_error(uint64_t fault_addr, uint64_t error_code) {
    struct thread *current = this_cpu() ? this_cpu()->current_thread : NULL;
    if (current && fault_addr >= current->kernel_stack && 
        fault_addr < (current->kernel_stack + 0x2000)) {
        return PF_TYPE_STACK_OVERFLOW;
    }
    
    /* 检查错误代码 */
    if (!(error_code & 1)) {
        return PF_TYPE_NOT_PRESENT;  /* 页不存在 */
    }
    
    if (error_code & (1 << 3)) {
        return PF_TYPE_PROTECTION;   /* 保留位错误 */
    }
    
    if (error_code & (1 << 2)) {
        return PF_TYPE_USER_ACCESS;  /* 用户模式错误 */
    }
    
    return PF_TYPE_UNKNOWN;
}

/* 检查是否为内核地址 */
int pf_is_kernel_address(uint64_t addr) {
    return addr >= 0xFFFFFFFF80000000ULL;
}

/* 检查是否为用户地址 */
int pf_is_user_address(uint64_t addr) {
    return addr < 0x800000000000ULL;
}

/* 检查是否为栈地址 */
int pf_is_stack_address(uint64_t addr, struct thread *thread) {
    if (!thread) return 0;
    
    uint64_t stack_top = thread->kernel_stack + 0x10000; // 假设栈大小
    uint64_t stack_bottom = thread->kernel_stack;
    
    return (addr >= stack_bottom && addr <= stack_top);
}

/* 预防栈溢出 */
int pf_prevent_stack_overflow(uint64_t fault_addr, struct thread *thread) {
    (void)fault_addr; (void)thread;  // 避免未使用变量警告
    return 0;  /* 暂时无法扩展栈 */
}

/* 预防无效访问 */
int pf_prevent_invalid_access(uint64_t fault_addr, uint64_t error_code) {
    /* 检查是否为NULL指针访问 */
    if (fault_addr < 0x1000) {
        return 0;  /* NULL指针访问，无法修复 */
    }
    
    /* 检查是否为内核访问用户空间 */
    if (pf_is_kernel_address(fault_addr) && (error_code & (1 << 2))) {
        return 0;  /* 内核访问用户空间，通常是错误 */
    }
    
    return 1;  /* 其他情况可能可以处理 */
}

/* 预防双重错误 */
int pf_prevent_double_fault(void) {
    return 1;  /* 简单返回，实际的双重错误检测在中断处理中完成 */
}

/* 处理栈溢出错误 */
pf_result_t handle_stack_overflow(struct interrupt_frame *frame, uint64_t fault_addr) {
    (void)frame; (void)fault_addr;  // 避免未使用参数警告
    // print("[PF] Stack overflow detected\n");
    return PF_HANDLED_FATAL;
}

/* 简化的用户页错误处理 */
pf_result_t handle_user_page_fault(struct interrupt_frame *frame, uint64_t fault_addr) {
    /* 检查 null pointer 解引用 */
    if (fault_addr < 0x1000) {
        serial_print("[PF] FATAL: Null pointer dereference at 0x");
        char buf[20];
        for (int k = 15; k >= 0; k--) { buf[15-k] = "0123456789ABCDEF"[(fault_addr >> (k*4)) & 0xF]; }
        buf[16] = '\0';
        serial_print(buf);
        serial_print("\n");
        pf_stats_record_typed(PF_TYPE_ACCESS_VIOLATION, 0);
        return PF_HANDLED_FATAL;
    }
    
    /* 获取页面级锁，防止 SMP 竞态 */
    spinlock_t *page_lock = get_pf_page_lock(fault_addr);
    
    /* 首先尝试增强的 COW 处理 */
    if ((frame->err_code & (1 << 1)) && (frame->err_code & 1)) {
        /* 写操作 + 页存在 = 可能的 COW 缺页 */
        int cow_result = pf_handle_cow_fault(fault_addr, frame->err_code);
        if (cow_result == 0) {
            pf_stats_record_typed(PF_TYPE_COW, 1);
            return PF_HANDLED_SUCCESS;
        }
        
        /* 回退到原有的 COW 处理路径 */
        spin_lock(page_lock);
        int cow_fallback = cow_handle_page_fault(fault_addr, frame->err_code);
        spin_unlock(page_lock);
        
        if (cow_fallback == 0) {
            spin_lock(&g_pf_stats_lock);
            g_pf_stats.handled_faults++;
            if (g_pf_stats.unhandled_faults > 0) g_pf_stats.unhandled_faults--;
            g_pf_stats.cow_faults++;
            spin_unlock(&g_pf_stats_lock);
            pf_stats_record_typed(PF_TYPE_COW, 1);
            return PF_HANDLED_SUCCESS;
        }
    }
    
    /* 尝试按需分页 - 使用当前进程空间 */
    if (!(frame->err_code & 1)) {
        struct address_space *space = vmm_get_current_space();
        if (space) {
            /* 页不存在，尝试按需分页 */
            uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
            if (frame->err_code & (1 << 1)) {
                flags |= VMM_FLAG_WRITABLE;
            }

            /* 使用锁保护页表创建 */
            spin_lock(page_lock);
            int create_result = vmm_create_pte_if_not_present(space, fault_addr, flags);
            spin_unlock(page_lock);
            
            if (create_result == 0) {
                spin_lock(&g_pf_stats_lock);
                g_pf_stats.handled_faults++;
                if (g_pf_stats.unhandled_faults > 0) g_pf_stats.unhandled_faults--;
                g_pf_stats.demand_paging_faults++;
                spin_unlock(&g_pf_stats_lock);
                
                /* 预取相邻页面 */
                pf_prefetch_adjacent(fault_addr, frame->err_code, PF_PREFETCH_FORWARD);
                
                pf_stats_record_typed(PF_TYPE_DEMAND_PAGING, 1);
                return PF_HANDLED_SUCCESS;
            }

            /* 尝试查找VMA */
            struct vm_area_struct *vma = vma_find(space, fault_addr);
            if (vma) {
                spin_lock(page_lock);
                int mmap_result = mmap_handle_fault(vma, fault_addr, frame->err_code);
                spin_unlock(page_lock);
                
                if (mmap_result == 0) {
                    spin_lock(&g_pf_stats_lock);
                    g_pf_stats.handled_faults++;
                    if (g_pf_stats.unhandled_faults > 0) g_pf_stats.unhandled_faults--;
                    spin_unlock(&g_pf_stats_lock);
                    
                    /* 预取相邻页面 */
                    pf_prefetch_adjacent(fault_addr, frame->err_code, PF_PREFETCH_FORWARD);
                    
                    pf_stats_record_typed(PF_TYPE_NOT_PRESENT, 1);
                    return PF_HANDLED_SUCCESS;
                }
            }
        }
    }
    
    pf_stats_record_typed(PF_TYPE_UNKNOWN, 0);
    return PF_HANDLED_FAILED;
}

/* 简化的内核页错误处理 */
pf_result_t handle_kernel_page_fault(struct interrupt_frame *frame, uint64_t fault_addr) {
    /* 内核页错误通常更严重，需要更仔细的分析 */
    // print("[PF] Kernel page fault at address: 0x");
    // print_hex(fault_addr);
    // print(", error: 0x");
    // print_hex(frame->err_code);
    // print(", RIP: 0x");
    // print_hex(frame->rip);
    // println("");
    
    /* 检查错误代码 - U/S 位 (bit 2) 表示访问模式 */
    int is_user_mode_access = frame->err_code & 4;
    
    /* 检查是否是内核模式下访问用户空间地址 */
    if (!is_user_mode_access && fault_addr < 0xFFFFFFFF80000000ULL) {
        // print("[PF] Kernel mode attempted access to user space address: 0x");
        // print_hex(fault_addr);
        // println("");
        
        /* This could be a legitimate case where kernel is accessing user space through system call */
        /* In such cases, we need to validate the user space address properly */
        struct process *current_proc = get_current_task();
        if (current_proc && current_proc->space) {
            /* Try to validate that the address is accessible in the current process space */
            /* But be careful to not cause recursive page faults */
            // print("[PF] Current process has valid address space, this may be a syscall issue\n");
            
            /* Check if this is a low address that's likely a null pointer */
            if (fault_addr < 0x1000) {
                // print("[PF] Null pointer access detected, returning fatal\n");
                return PF_HANDLED_FATAL;
            }
            
            /* 对于内核访问用户空间的情况，尝试跳过 */
            if (pf_can_skip_fault(fault_addr, frame->rip, frame->err_code)) {
                // print("[PF] Attempting to skip faulting instruction\n");
                if (pf_skip_instruction(frame) == 0) {
                    return PF_SKIPPED;
                }
            }
            
            /* For other user space accesses from kernel, return FAILED to allow other handlers */
            return PF_HANDLED_FAILED;
        } else {
            // print("[PF] No valid current process, returning fatal\n");
            return PF_HANDLED_FATAL;
        }
    }
    
    return PF_HANDLED_FAILED;
}

/* 主要页错误处理函数 */
pf_result_t handle_page_fault(struct interrupt_frame *frame, uint64_t fault_addr) {
    pf_type_t type = pf_classify_error(fault_addr, frame->err_code);
    
    /* 记录错误类型 */
    pf_stats_record_fault(type);
    
    /* 检查是否是栈溢出 */
    if (type == PF_TYPE_STACK_OVERFLOW) {
        return handle_stack_overflow(frame, fault_addr);
    }
    
    /* 检查是否是用户模式访问 */
    if (frame->err_code & 4) {  /* 用户模式 */
        return handle_user_page_fault(frame, fault_addr);
    } else {  /* 内核模式 */
        pf_result_t result = handle_kernel_page_fault(frame, fault_addr);
        /* 如果跳过成功，返回 SKIPPED */
        if (result == PF_SKIPPED) {
            return PF_SKIPPED;
        }
        return result;
    }
}

/* 安全终止进程 */
int pf_safe_terminate_process(struct process *proc, const char *reason) {
    if (!proc) return -1;
    
    // print("[PF] Terminating process PID ");
    // print_dec(proc->pid);
    // print(" (");
    print((char *)reason);
    // println(")");
    
    return 0;  /* 简单返回，实际的进程终止在调用者中处理 */
}

/* 页错误调试信息 */
void pf_debug_info(struct page_fault_info *info) {
    // print("[PF DEBUG] Fault Info:\n");
    // print("  Address: 0x"); print_hex(info->fault_addr); println("");
    // print("  Error: 0x"); print_hex(info->error_code); println("");
    // print("  RIP: 0x"); print_hex(info->rip); println("");
    // println("");
}

/* 转储上下文 */
void pf_dump_context(struct interrupt_frame *frame, uint64_t fault_addr) {
    // print("[PF] Context dump:\n");
    // print("  Fault address: 0x"); print_hex(fault_addr); println("");
    // print("  Error code: 0x"); print_hex(frame->err_code); println("");
    // print("  RIP: 0x"); print_hex(frame->rip); println("");
    // print("  RSP: 0x"); print_hex(frame->rsp); println("");
    // print("  RBP: 0x"); print_hex(frame->rbp); println("");
    // print("  RAX: 0x"); print_hex(frame->rax); println("");
    // print("  RBX: 0x"); print_hex(frame->rbx); println("");
    // print("  RCX: 0x"); print_hex(frame->rcx); println("");
    // print("  RDX: 0x"); print_hex(frame->rdx); println("");
}

/* 页错误恢复 */
void pf_try_recovery(struct page_fault_info *info) {
    (void)info;  // 避免未使用参数警告
    // print("[PF] Recovery not implemented for this case\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 指令跳过功能 - 允许内核从某些页错误中恢复
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 获取指令长度 - 简化版 x86-64 指令解码器 */
uint64_t pf_get_instruction_length(uint64_t rip) {
    uint8_t *code = (uint8_t *)rip;
    uint64_t len = 0;
    
    /* 安全检查: 确保 RIP 在有效的内核地址范围内 */
    if (rip < 0xFFFFFFFF80000000ULL) {
        return 0;  /* 无效地址 */
    }
    
    /* 使用异常处理机制安全读取内存 - 每次读取一个字节 */
    uint8_t bytes[16] = {0};
    int read_ok = 1;
    
    for (int i = 0; i < 16 && read_ok; i++) {
        __asm__ volatile (
            "1: movb (%[src]), %[dst]\n\t"
            "jmp 2f\n\t"
            ".section .text.fixup, \"ax\"\n\t"
            "3: movb $0, %[dst]\n\t"
            "movl $0, %[ok]\n\t"
            "jmp 2f\n\t"
            ".previous\n\t"
            ".section __ex_table, \"a\"\n\t"
            ".quad 1b, 3b\n\t"
            ".previous\n\t"
            "2:"
            : [dst] "=r" (bytes[i]), [ok] "+r" (read_ok)
            : [src] "r" (&code[i])
            : "memory"
        );
    }
    
    if (!read_ok) {
        return 0;  /* 无法读取指令 */
    }
    
    code = bytes;
    
    /* 跳过 Legacy Prefixes */
    while (len < 15) {
        switch (code[len]) {
            case 0xF0:  /* LOCK */
            case 0xF2:  /* REPNE */
            case 0xF3:  /* REP/REPE */
            case 0x2E:  /* CS override */
            case 0x36:  /* SS override */
            case 0x3E:  /* DS override */
            case 0x26:  /* ES override */
            case 0x64:  /* FS override */
            case 0x65:  /* GS override */
            case 0x66:  /* Operand size override */
            case 0x67:  /* Address size override */
                len++;
                break;
            default:
                goto prefixes_done;
        }
    }
    
prefixes_done:
    
    /* 检查 REX prefix (0x40-0x4F) */
    int rex_w = 0;
    if (len < 15 && (code[len] & 0xF0) == 0x40) {
        rex_w = (code[len] >> 3) & 1;
        len++;
    }
    
    /* 操作码 */
    if (len >= 15) return 0;
    uint8_t opcode = code[len++];
    
    /* 处理双字节操作码 */
    if (opcode == 0x0F) {
        if (len >= 15) return 0;
        opcode = code[len++];  /* 第二字节 */
        
        /* 简化处理常见双字节指令 */
        switch (opcode) {
            case 0xB6:  /* MOVZX r64, r/m8 */
            case 0xB7:  /* MOVZX r64, r/m16 */
            case 0xBE:  /* MOVSX r64, r/m8 */
            case 0xBF:  /* MOVSX r64, r/m16 */
                /* 有 ModR/M */
                goto has_modrm;
            default:
                /* 大多数双字节指令有 ModR/M */
                goto has_modrm;
        }
    }
    
    /* 单字节操作码分析 */
    switch (opcode) {
        /* MOV r64, r/m64 或 MOV r/m64, r64 */
        case 0x8A:  /* MOV r8, r/m8 */
        case 0x8B:  /* MOV r64, r/m64 */
        case 0x8C:  /* MOV r/m64, Sreg */
        case 0x8E:  /* MOV Sreg, r/m16 */
        case 0x88:  /* MOV r/m8, r8 */
        case 0x89:  /* MOV r/m64, r64 */
has_modrm:
            if (len >= 15) return 0;
            {
                uint8_t modrm = code[len++];
                uint8_t mod = (modrm >> 6) & 3;
                uint8_t rm = modrm & 7;
                
                if (mod == 3) {
                    /* 寄存器到寄存器，无额外字节 */
                } else if (mod == 0 && rm == 5) {
                    /* RIP 相对寻址，32位位移 */
                    len += 4;
                } else if (mod == 0 && rm == 4) {
                    /* SIB */
                    if (len >= 15) return 0;
                    uint8_t sib = code[len++];
                    if ((sib & 7) == 5) {
                        len += 4;  /* 带位移 */
                    }
                } else if (mod == 1) {
                    len += 1;  /* 8位位移 */
                } else if (mod == 2) {
                    len += 4;  /* 32位位移 */
                }
            }
            break;
            
        /* CMP 指令 */
        case 0x3A:  /* CMP r8, r/m8 */
        case 0x3B:  /* CMP r64, r/m64 */
        case 0x38:  /* CMP r/m8, r8 */
        case 0x39:  /* CMP r/m64, r64 */
            goto has_modrm;
            
        /* TEST 指令 */
        case 0x84:  /* TEST r/m8, r8 */
        case 0x85:  /* TEST r/m64, r64 */
            goto has_modrm;
            
        /* XCHG */
        case 0x86:  /* XCHG r/m8, r8 */
        case 0x87:  /* XCHG r/m64, r64 */
            goto has_modrm;
            
        /* ADD, OR, ADC, SBB, AND, SUB, XOR, CMP r/m, imm */
        case 0x80:  /* r/m8, imm8 */
        case 0x81:  /* r/m64, imm32 */
        case 0x83:  /* r/m64, imm8 (sign extended) */
            if (len >= 15) return 0;
            {
                uint8_t modrm = code[len++];
                uint8_t mod = (modrm >> 6) & 3;
                uint8_t rm = modrm & 7;
                
                if (mod == 3) {
                    /* 寄存器 */
                } else if (mod == 0 && rm == 5) {
                    len += 4;
                } else if (mod == 0 && rm == 4) {
                    if (len >= 15) return 0;
                    len++;
                    uint8_t sib = code[len - 1];
                    if ((sib & 7) == 5) len += 4;
                } else if (mod == 1) {
                    len++;
                } else if (mod == 2) {
                    len += 4;
                }
                
                /* 立即数 */
                if (opcode == 0x80) len += 1;
                else if (opcode == 0x81) len += 4;
                else if (opcode == 0x83) len += 1;
            }
            break;
            
        /* MOV r/m64, imm64 */
        case 0xC7:
            if (len >= 15) return 0;
            {
                uint8_t modrm = code[len++];
                uint8_t mod = (modrm >> 6) & 3;
                uint8_t rm = modrm & 7;
                
                if (mod == 0 && rm == 5) len += 4;
                else if (mod == 0 && rm == 4) {
                    if (len >= 15) return 0;
                    len++;
                }
                else if (mod == 1) len++;
                else if (mod == 2) len += 4;
                
                len += 4;  /* imm32 */
            }
            break;
            
        /* 单字节指令 */
        case 0x90:  /* NOP */
        case 0xCC:  /* INT3 */
        case 0xC3:  /* RET */
        case 0xEB:  /* JMP rel8 */
            break;
            
        /* 带立即数的指令 */
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:  /* MOV r8, imm8 */
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            len += 1;
            break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:  /* MOV r64, imm64 (or imm32 with REX.W=0) */
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            if (rex_w) len += 8;  /* REX.W = 64-bit immediate */
            else len += 4;        /* 32-bit immediate */
            break;
            
        /* PUSH/POP */
        case 0x50: case 0x51: case 0x52: case 0x53:  /* PUSH r64 */
        case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B:  /* POP r64 */
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            break;
            
        /* CALL/JMP */
        case 0xE8:  /* CALL rel32 */
        case 0xE9:  /* JMP rel32 */
            len += 4;
            break;
        case 0xFF:  /* INC/DEC/CALL/JMP/PUSH r/m64 */
            goto has_modrm;
            
        /* LEA */
        case 0x8D:  /* LEA r64, m */
            goto has_modrm;
            
        /* 不支持的指令，返回0 */
        default:
            return 0;
    }
    
    return len;
}

/* 检查是否可以安全跳过该页错误 */
int pf_can_skip_fault(uint64_t fault_addr, uint64_t rip, uint64_t error_code) {
    /* 只能跳过内核模式访问用户空间地址的情况 */
    /* U/S = 0 表示内核模式 */
    if (error_code & 4) {
        return 0;  /* 用户模式错误，不能跳过 */
    }
    
    /* 只处理访问用户空间地址的情况 */
    if (fault_addr >= 0x800000000000ULL) {
        return 0;  /* 不是用户空间地址 */
    }
    
    /* RIP 必须在内核空间 */
    if (rip < 0xFFFFFFFF80000000ULL) {
        return 0;  /* RIP 不在内核空间 */
    }
    
    /* 不能跳过 NULL 指针访问 */
    if (fault_addr < 0x1000) {
        return 0;
    }
    
    /* 这是内核访问用户空间的情况，可以尝试跳过 */
    return 1;
}

/* 跳过当前指令并继续执行 */
int pf_skip_instruction(struct interrupt_frame *frame) {
    uint64_t instr_len = pf_get_instruction_length(frame->rip);
    
    if (instr_len == 0) {
        // print("[PF] Cannot determine instruction length at RIP: 0x");
        // print_hex(frame->rip);
        // println("");
        return -1;
    }
    
    /* 更新统计 */
    spin_lock(&g_pf_stats_lock);
    g_pf_stats.skipped_faults++;
    g_pf_stats.skipped_instructions++;
    g_pf_stats.handled_faults++;
    if (g_pf_stats.unhandled_faults > 0) {
        g_pf_stats.unhandled_faults--;
    }
    spin_unlock(&g_pf_stats_lock);
    
    /* 跳过指令 */
    frame->rip += instr_len;
    
    // print("[PF] Skipped instruction at 0x");
    // print_hex(frame->rip - instr_len);
    // print(", new RIP: 0x");
    // print_hex(frame->rip);
    // print(", instruction length: ");
    // print_dec(instr_len);
    // println("");
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * COW 缺页处理增强
 * 检测对只读 COW 页的写入，分配新页，复制数据，更新 PTE
 * ═══════════════════════════════════════════════════════════════════════════ */

int pf_handle_cow_fault(uint64_t fault_addr, uint64_t error_code) {
    /* COW 缺页条件: 页存在 (P=1) + 写操作 (W/R=1) */
    if (!(error_code & PF_ERROR_PRESENT) || !(error_code & PF_ERROR_WRITE)) {
        return -1;  /* 不是 COW 缺页 */
    }

    /* 对齐到页边界 */
    uint64_t page_addr = fault_addr & ~0xFFFULL;

    /* 获取页面级锁防止 SMP 竞态 */
    spinlock_t *page_lock = get_pf_page_lock(page_addr);
    spin_lock(page_lock);

    /* 首先检查是否为 COW 页 */
    struct address_space *space = vmm_get_current_space();
    if (!space) {
        spin_unlock(page_lock);
        return -1;
    }

    if (!cow_is_cow_page(space, page_addr)) {
        spin_unlock(page_lock);
        return -1;  /* 不是 COW 页 */
    }

    /* 调用 COW 处理: 分配新页、复制数据、更新 PTE */
    int result = cow_handle_page_fault(page_addr, error_code);
    if (result != 0) {
        spin_unlock(page_lock);
        return -1;
    }

    /* 更新统计 */
    spin_lock(&g_pf_stats_lock);
    g_pf_stats.cow_faults++;
    g_pf_stats.cow_breaks++;
    g_pf_stats.handled_faults++;
    if (g_pf_stats.unhandled_faults > 0) {
        g_pf_stats.unhandled_faults--;
    }
    spin_unlock(&g_pf_stats_lock);

    spin_unlock(page_lock);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 页错误预取
 * 当缺页发生时，预取相邻页面以减少后续缺页次数
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 预取配置 */
#define PF_PREFETCH_FORWARD     2    /* 前向预取页数 */
#define PF_PREFETCH_BACKWARD    1    /* 后向预取页数 */
#define PF_PREFETCH_MAX         4    /* 最大预取页数 */

void pf_prefetch_adjacent(uint64_t fault_addr, uint64_t error_code, int prefetch_pages) {
    if (prefetch_pages <= 0) return;
    if (prefetch_pages > PF_PREFETCH_MAX) {
        prefetch_pages = PF_PREFETCH_MAX;
    }

    /* 只对按需分页 (页不存在) 进行预取，不对 COW 或保护错误预取 */
    if (error_code & PF_ERROR_PRESENT) {
        return;  /* 页已存在，不需要预取 */
    }

    struct address_space *space = vmm_get_current_space();
    if (!space) return;

    uint64_t base_page = fault_addr & ~0xFFFULL;

    spin_lock(&g_pf_stats_lock);
    g_pf_stats.prefetch_attempts += prefetch_pages;
    spin_unlock(&g_pf_stats_lock);

    /* 前向预取: 预取 fault_addr 之后的页面 */
    for (int i = 1; i <= prefetch_pages; i++) {
        uint64_t next_addr = base_page + (uint64_t)i * 0x1000;

        /* 检查地址是否在用户空间范围内 */
        if (next_addr >= 0x800000000000ULL) break;

        /* 检查 VMA 是否存在 (地址是否合法) */
        struct vm_area_struct *vma = vma_find(space, next_addr);
        if (!vma) break;

        /* 检查页是否已存在 */
        spinlock_t *page_lock = get_pf_page_lock(next_addr);
        spin_lock(page_lock);

        int create_result = vmm_create_pte_if_not_present(space, next_addr,
            VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE);
        spin_unlock(page_lock);

        if (create_result == 0) {
            spin_lock(&g_pf_stats_lock);
            g_pf_stats.prefetch_hits++;
            spin_unlock(&g_pf_stats_lock);
        } else {
            break;  /* 预取失败，停止继续预取 */
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 页错误详细统计
 * 按类型记录页错误计数，用于性能分析和调试
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 记录按类型统计的页错误 */
void pf_stats_record_typed(pf_type_t type, int handled) {
    int idx = (int)type;
    if (idx < 0 || idx >= 16) return;

    spin_lock(&g_pf_stats_lock);
    pf_type_counts[idx]++;
    if (handled) {
        pf_type_handled[idx]++;
    }
    spin_unlock(&g_pf_stats_lock);
}

/* 获取按类型的页错误计数 */
uint64_t pf_stats_get_count(pf_type_t type) {
    int idx = (int)type;
    if (idx < 0 || idx >= 16) return 0;
    return pf_type_counts[idx];
}

/* 重置页错误统计 */
void pf_stats_reset(void) {
    spin_lock(&g_pf_stats_lock);
    memset(&g_pf_stats, 0, sizeof(g_pf_stats));
    memset(pf_type_counts, 0, sizeof(pf_type_counts));
    memset(pf_type_handled, 0, sizeof(pf_type_handled));
    spin_unlock(&g_pf_stats_lock);
}