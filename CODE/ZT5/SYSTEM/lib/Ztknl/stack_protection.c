/**
 * @file stack_protection.c
 * @brief 内核栈保护机制
 * 
 * 提供栈 canary 支持，防止栈缓冲区溢出攻击。
 * 使用 rdtsc 作为 canary 随机源。
 */

#include "stack_protection.h"
#include "cpu.h"
#include "console.h"
#include "string.h"

/* 全局栈 canary 值 */
uint64_t __stack_chk_guard = STACK_CANARY_VALUE;

/* 每个进程的栈 canary 值存储区 */
#define MAX_STACK_CANARIES 256
static uint64_t process_canaries[MAX_STACK_CANARIES];
static int canary_count = 0;

/* 初始化栈保护 */
void stack_protection_init(void) {
    /* 使用 TSC 作为随机源初始化 canary */
    uint64_t tsc = rdtsc();
    
    /* 混合 TSC 值生成更强的随机性 */
    __stack_chk_guard = tsc ^ (tsc >> 32) ^ 0xDEADBEEFCAFEBABEULL;
    
    /* 如果 TSC 为 0，使用默认值 */
    if (__stack_chk_guard == 0) {
        __stack_chk_guard = STACK_CANARY_VALUE;
    }
    
    memset(process_canaries, 0, sizeof(process_canaries));
    canary_count = 0;
    
    /* 初始化栈统计 (来自 task.c) */
    extern void stack_stats_init(void);
    stack_stats_init();
    
    // println("[STACK] Stack protection initialized");
    // print("  [STACK] Global canary: ");
    // print_hex(__stack_chk_guard);
    // println("");
}

/* 为进程分配栈 canary */
uint64_t stack_canary_alloc(void) {
    if (canary_count >= MAX_STACK_CANARIES) {
        return __stack_chk_guard;  /* 使用全局 canary */
    }
    
    /* 基于进程 ID 和 TSC 生成唯一 canary */
    uint64_t tsc = rdtsc();
    uint64_t canary = tsc ^ ((uint64_t)(canary_count + 1) << 32);
    
    process_canaries[canary_count++] = canary;
    return canary;
}

/* 释放进程栈 canary */
void stack_canary_free(uint64_t canary) {
    for (int i = 0; i < canary_count; i++) {
        if (process_canaries[i] == canary) {
            process_canaries[i] = 0;
            break;
        }
    }
}

/* 验证栈 canary */
int stack_canary_verify(uint64_t canary) {
    /* 检查是否匹配全局 canary */
    if (canary == __stack_chk_guard) {
        return 1;
    }
    
    /* 检查是否匹配任何进程 canary */
    for (int i = 0; i < canary_count; i++) {
        if (process_canaries[i] == canary) {
            return 1;
        }
    }
    
    return 0;
}

/* Stack canary 设置 - 编译器调用 */
void __stack_chk_setup(void) {
    /* 编译器生成的代码会在函数入口调用此函数 */
    /* 当前使用全局 canary */
}

/* Stack canary 失败处理 - 编译器调用 */
void __stack_chk_fail(void) {
    // println("");
    // println("╔════════════════════════════════════════════════════════════╗");
    // println("║           STACK BUFFER OVERFLOW DETECTED!                  ║");
    // println("║                                                             ║");
    // println("║   System halted for security reasons.                      ║");
    // println("╚════════════════════════════════════════════════════════════╝");
    // println("");
    
    /* 记录安全事件 */
    // TODO: 记录到安全日志
    
    /* 停止系统 */
    cli();
    while (1) {
        hlt();
    }
}

/* Stack canary 失败处理 (带返回地址) */
void __stack_chk_fail_local(void) {
    __stack_chk_fail();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CET Shadow Stack 支持
 * Intel CET (Control-flow Enforcement Technology) 提供硬件级返回地址保护
 * Shadow Stack 将返回地址的副本存储在独立的内存区域中
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Shadow Stack 状态 */
static uint8_t shadow_stack_area[SHADOW_STACK_SIZE] __attribute__((aligned(16)));
static int shadow_stack_enabled = 0;
static int cet_available = -1;  /* -1=未检测, 0=不支持, 1=支持 */

/* 检查 CPU 是否支持 CET Shadow Stack */
int stack_cet_supported(void) {
    if (cet_available >= 0) return cet_available;

    /* CPUID leaf 7, subleaf 0, ECX bit 7 = CET IBT, EDX bit 20 = CET Shadow Stack */
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(7), "c"(0));

    /* EDX bit 20 = Shadow Stack 支持 */
    if (edx & (1 << 20)) {
        cet_available = 1;
    } else {
        cet_available = 0;
    }

    return cet_available;
}

/* 初始化 Shadow Stack (每个 CPU 调用一次) */
int stack_shadow_stack_init(void) {
    if (!stack_cet_supported()) {
        return -1;  /* 硬件不支持 */
    }

    /* 清零 Shadow Stack 区域 */
    memset(shadow_stack_area, 0, sizeof(shadow_stack_area));

    /* 设置 Shadow Stack 指针 (PL0_SSP MSR = 0x6A2)
     * 指向 shadow_stack_area 的顶部 (栈向下增长) */
    uint64_t ssp = (uint64_t)&shadow_stack_area[SHADOW_STACK_SIZE];
    ssp &= ~0x3ULL;  /* 必须对齐到 4 字节 */

    /* 写入 PL0_SSP MSR */
    asm volatile("wrmsr" :: "a"((uint32_t)ssp), "d"((uint32_t)(ssp >> 32)), "c"(0x6A2));

    return 0;
}

/* 启用 Shadow Stack */
int stack_shadow_stack_enable(void) {
    if (!stack_cet_supported()) return -1;

    /* 设置 CR4.CET 位 (bit 23) 来启用 CET
     * 注意: 实际启用需要同时设置 CET 控制寄存器 */
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    /* 检查是否已启用 */
    if (cr4 & (1ULL << 23)) {
        shadow_stack_enabled = 1;
        return 0;
    }

    /* 启用 CR4.CET */
    cr4 |= (1ULL << 23);
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    /* 设置 CET 控制 MSR (IA32_U_CET = 0x6A0)
     * bit 0 = SH_STK_EN (Shadow Stack 启用)
     * bit 2 = WR_SHSTK_EN (允许写入 Shadow Stack) */
    uint64_t cet_ctrl = 0x05;  /* SH_STK_EN | WR_SHSTK_EN */
    asm volatile("wrmsr" :: "a"((uint32_t)cet_ctrl), "d"((uint32_t)(cet_ctrl >> 32)), "c"(0x6A0));

    /* 设置 Ring-0 Shadow Stack 指针 */
    int init_result = stack_shadow_stack_init();
    if (init_result != 0) {
        /* 回滚: 禁用 CR4.CET */
        cr4 &= ~(1ULL << 23);
        asm volatile("mov %0, %%cr4" :: "r"(cr4));
        return -1;
    }

    shadow_stack_enabled = 1;
    return 0;
}

/* 禁用 Shadow Stack */
void stack_shadow_stack_disable(void) {
    if (!shadow_stack_enabled) return;

    /* 清除 CET 控制寄存器 */
    uint64_t cet_ctrl = 0;
    asm volatile("wrmsr" :: "a"((uint32_t)cet_ctrl), "d"((uint32_t)(cet_ctrl >> 32)), "c"(0x6A0));

    /* 清除 CR4.CET 位 */
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 &= ~(1ULL << 23);
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    shadow_stack_enabled = 0;
}

/* 检查 Shadow Stack 是否已启用 */
int stack_shadow_stack_is_enabled(void) {
    return shadow_stack_enabled;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stack Canary 增强验证
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 在函数返回时验证 canary */
int stack_canary_check_on_return(uint64_t saved_canary, uint64_t expected_canary) {
    if (saved_canary != expected_canary) {
        /* Canary 不匹配 - 栈已被破坏 */
        serial_print("[STACK] Canary verification failed! Stack corruption detected.\n");
        return -1;
    }
    return 0;
}

/* 获取当前线程的 canary 值 */
uint64_t stack_get_current_canary(void) {
    return __stack_chk_guard;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 栈回溯 (Backtrace) 工具
 * 遍历栈帧链收集返回地址，用于调试和错误报告
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 执行栈回溯 (从当前 RBP) */
int stack_backtrace(struct stack_trace *trace, int max_frames) {
    if (!trace || max_frames <= 0) return 0;

    /* 获取当前 RBP */
    uint64_t rbp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));

    return stack_backtrace_from(trace, rbp, max_frames);
}

/* 从指定帧指针执行栈回溯 */
int stack_backtrace_from(struct stack_trace *trace, uint64_t rbp, int max_frames) {
    if (!trace || max_frames <= 0) return 0;

    int count = 0;
    int limit = max_frames;
    if (limit > STACK_TRACE_MAX) limit = STACK_TRACE_MAX;

    struct stack_frame *frame = (struct stack_frame *)rbp;

    while (frame != NULL && count < limit) {
        /* 安全检查: 帧指针必须在内核地址空间 */
        if ((uint64_t)frame < 0xFFFFFFFF80000000ULL) {
            break;  /* 跳出内核空间，停止回溯 */
        }

        /* 检查帧指针对齐 */
        if ((uint64_t)frame & 0x7) {
            break;  /* 帧指针未对齐，可能已损坏 */
        }

        /* 检查 next 指针是否合理 (应大于当前帧) */
        if (frame->next != NULL && (uint64_t)frame->next <= (uint64_t)frame) {
            break;  /* 栈帧不应向下增长 */
        }

        /* 记录返回地址 */
        trace->frames[count] = frame->return_addr;
        count++;

        /* 移动到上一级帧 */
        frame = frame->next;
    }

    trace->count = count;
    return count;
}

/* 打印栈回溯 */
void stack_print_backtrace(struct stack_trace *trace) {
    if (!trace || trace->count <= 0) {
        serial_print("[STACK] No backtrace available\n");
        return;
    }

    serial_print("[STACK] Backtrace:\n");
    for (int i = 0; i < trace->count; i++) {
        serial_print("  [");
        /* 简单的十进制转字符串 */
        char idx_buf[8];
        int p = 0;
        int v = i;
        if (v == 0) idx_buf[p++] = '0';
        else { char t[8]; int tp = 0; while (v > 0) { t[tp++] = '0' + v % 10; v /= 10; } while (tp > 0) idx_buf[p++] = t[--tp]; }
        idx_buf[p] = '\0';
        serial_print(idx_buf);
        serial_print("] 0x");

        /* 十六进制输出返回地址 */
        char addr_buf[20];
        for (int k = 15; k >= 0; k--) {
            addr_buf[15 - k] = "0123456789ABCDEF"[(trace->frames[i] >> (k * 4)) & 0xF];
        }
        addr_buf[16] = '\0';
        serial_print(addr_buf);
        serial_print("\n");
    }
}
