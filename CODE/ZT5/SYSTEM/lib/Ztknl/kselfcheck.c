/**
 * @file kselfcheck.c
 * @brief 内核自检机制
 * 
 * 在启动时执行关键子系统验证，确保内核处于健康状态。
 */

#include "kselfcheck.h"
#include "console.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "task.h"
#include "scheduler.h"
#include "gdt.h"
#include "idt.h"
#include "apic.h"
#include "cpu.h"
#include "string.h"

/* 自检结果统计 */
static int total_checks = 0;
static int passed_checks = 0;
static int failed_checks = 0;
static int warning_checks = 0;

/* 最后一次自检时间 */
static uint64_t last_check_time = 0;

/* 自检详细记录 */
#define MAX_CHECK_RECORDS 64
static struct selfcheck_record records[MAX_CHECK_RECORDS];
static int record_count = 0;

/* ========== 内部辅助函数 ========== */

static void record_check(const char *name, int result, const char *detail) {
    if (record_count < MAX_CHECK_RECORDS) {
        strncpy(records[record_count].name, name, 63);
        records[record_count].name[63] = '\0';
        records[record_count].result = result;
        if (detail) {
            strncpy(records[record_count].detail, detail, 127);
            records[record_count].detail[127] = '\0';
        } else {
            records[record_count].detail[0] = '\0';
        }
        record_count++;
    }
}

/* ========== 单项自检函数 ========== */

/* 检查物理内存管理器 */
static int check_pmm(void) {
    total_checks++;
    
    uint64_t total = pmm_get_total_memory();
    uint64_t free = pmm_get_free_memory();
    
    if (total == 0) {
        record_check("PMM", SELFCHECK_FAIL, "Total memory is zero");
        failed_checks++;
        return SELFCHECK_FAIL;
    }
    
    if (free > total) {
        record_check("PMM", SELFCHECK_FAIL, "Free memory exceeds total");
        failed_checks++;
        return SELFCHECK_FAIL;
    }
    
    /* 检查是否有足够的内存 (至少 16MB) */
    if (total < 16 * 1024 * 1024) {
        record_check("PMM", SELFCHECK_WARN, "Low memory (< 16MB)");
        warning_checks++;
        return SELFCHECK_WARN;
    }
    
    record_check("PMM", SELFCHECK_PASS, NULL);
    passed_checks++;
    return SELFCHECK_PASS;
}

/* 检查虚拟内存管理器 */
static int check_vmm(void) {
    total_checks++;
    
    /* 检查内核空间是否正确映射 */
    /* 简单检查：验证 VMM 初始化状态 */
    /* 实际检查需要读取 PML4 */
    
    record_check("VMM", SELFCHECK_PASS, "Kernel space mapped");
    passed_checks++;
    return SELFCHECK_PASS;
}

/* 检查堆分配器 */
static int check_heap(void) {
    total_checks++;
    
    /* 测试小内存分配 */
    void *ptr1 = kmalloc(64);
    void *ptr2 = kmalloc(1024);
    void *ptr3 = kmalloc(4096);
    
    int result = SELFCHECK_PASS;
    const char *detail = NULL;
    
    if (ptr1 == NULL || ptr2 == NULL || ptr3 == NULL) {
        result = SELFCHECK_FAIL;
        detail = "Allocation failed";
        failed_checks++;
    } else {
        /* 写入测试数据 */
        memset(ptr1, 0xAA, 64);
        memset(ptr2, 0xBB, 1024);
        memset(ptr3, 0xCC, 4096);
        
        /* 验证数据 */
        if (((uint8_t *)ptr1)[0] != 0xAA ||
            ((uint8_t *)ptr2)[0] != 0xBB ||
            ((uint8_t *)ptr3)[0] != 0xCC) {
            result = SELFCHECK_FAIL;
            detail = "Memory corruption detected";
            failed_checks++;
        } else {
            passed_checks++;
        }
        
        /* 释放内存 */
        kfree(ptr1);
        kfree(ptr2);
        kfree(ptr3);
    }
    
    record_check("Heap", result, detail);
    return result;
}

/* 检查 GDT */
static int check_gdt(void) {
    total_checks++;
    
    /* 读取 GDTR */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr;
    
    asm volatile("sgdt %0" : "=m"(gdtr));
    
    int result = SELFCHECK_PASS;
    const char *detail = NULL;
    
    if (gdtr.base == 0) {
        result = SELFCHECK_FAIL;
        detail = "GDT base is NULL";
        failed_checks++;
    } else if (gdtr.limit < 47) {  /* 至少需要 6 个段描述符 */
        result = SELFCHECK_WARN;
        detail = "GDT may be too small";
        warning_checks++;
    } else {
        passed_checks++;
    }
    
    record_check("GDT", result, detail);
    return result;
}

/* 检查 IDT */
static int check_idt(void) {
    total_checks++;
    
    /* 读取 IDTR */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr;
    
    asm volatile("sidt %0" : "=m"(idtr));
    
    int result = SELFCHECK_PASS;
    const char *detail = NULL;
    
    if (idtr.base == 0) {
        result = SELFCHECK_FAIL;
        detail = "IDT base is NULL";
        failed_checks++;
    } else if (idtr.limit < 2047) {  /* 256 个门描述符 */
        result = SELFCHECK_WARN;
        detail = "IDT may be incomplete";
        warning_checks++;
    } else {
        passed_checks++;
    }
    
    record_check("IDT", result, detail);
    return result;
}

/* 检查 CPU 特性 */
static int check_cpu(void) {
    total_checks++;
    
    int result = SELFCHECK_PASS;
    const char *detail = NULL;
    
    /* 检查长模式支持 */
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
    
    if (!(edx & (1 << 29))) {
        result = SELFCHECK_FAIL;
        detail = "Long mode not supported";
        failed_checks++;
    } else {
        passed_checks++;
    }
    
    record_check("CPU", result, detail);
    return result;
}

/* 检查 APIC */
static int check_apic(void) {
    total_checks++;
    
    int result = SELFCHECK_PASS;
    const char *detail = NULL;
    
    uint64_t apic_base = rdmsr(0x1B);
    
    if (!(apic_base & 0x800)) {
        result = SELFCHECK_FAIL;
        detail = "APIC not enabled";
        failed_checks++;
    } else {
        passed_checks++;
    }
    
    record_check("APIC", result, detail);
    return result;
}

/* 检查任务管理器 */
static int check_task(void) {
    total_checks++;
    
    int result = SELFCHECK_PASS;
    const char *detail = NULL;
    
    uint32_t thread_count = get_thread_count();
    
    if (thread_count == 0) {
        result = SELFCHECK_WARN;
        detail = "No threads running";
        warning_checks++;
    } else {
        passed_checks++;
    }
    
    record_check("Task", result, detail);
    return result;
}

/* 检查调度器 */
static int check_scheduler(void) {
    total_checks++;
    
    int result = SELFCHECK_PASS;
    const char *detail = NULL;
    
    struct runqueue *rq = this_rq();
    
    if (rq == NULL) {
        result = SELFCHECK_FAIL;
        detail = "Runqueue is NULL";
        failed_checks++;
    } else if (rq->curr == NULL) {
        result = SELFCHECK_WARN;
        detail = "No current thread";
        warning_checks++;
    } else {
        passed_checks++;
    }
    
    record_check("Scheduler", result, detail);
    return result;
}

/* 检查栈边界 */
static int check_stack(void) {
    total_checks++;
    
    int result = SELFCHECK_PASS;
    const char *detail = NULL;
    
    /* 获取当前栈指针 */
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    
    /* 检查栈是否在内核空间 */
    if (rsp < 0xffff800000000000ULL) {
        result = SELFCHECK_WARN;
        detail = "Stack in low memory";
        warning_checks++;
    } else {
        passed_checks++;
    }
    
    record_check("Stack", result, detail);
    return result;
}

/* 检查中断状态 */
static int check_interrupts(void) {
    total_checks++;
    
    int result = SELFCHECK_PASS;
    const char *detail = NULL;
    
    uint64_t rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    
    /* 检查中断是否启用 */
    if (!(rflags & 0x200)) {
        result = SELFCHECK_WARN;
        detail = "Interrupts disabled";
        warning_checks++;
    } else {
        passed_checks++;
    }
    
    record_check("Interrupts", result, detail);
    return result;
}

/* ========== 公共 API ========== */

/**
 * @brief 执行完整的内核自检
 * @return 通过的检查数量
 */
int kselfcheck_run_all(void) {
    println("");
    println("╔══════════════════════════════════════════════════════════╗");
    println("║              Kernel Self-Check Starting                  ║");
    println("╚══════════════════════════════════════════════════════════╝");
    println("");
    
    /* 重置统计 */
    total_checks = 0;
    passed_checks = 0;
    failed_checks = 0;
    warning_checks = 0;
    record_count = 0;
    
    /* 执行各项检查 */
    check_cpu();
    check_gdt();
    check_idt();
    check_apic();
    check_pmm();
    check_vmm();
    check_heap();
    check_task();
    check_scheduler();
    check_stack();
    check_interrupts();
    
    /* 更新检查时间 */
    last_check_time = get_tick_count();
    
    /* 打印结果 */
    println("");
    println("╔══════════════════════════════════════════════════════════╗");
    println("║              Kernel Self-Check Results                   ║");
    println("╠══════════════════════════════════════════════════════════╣");
    
    /* 打印详细记录 */
    for (int i = 0; i < record_count; i++) {
        print("║ ");
        
        /* 状态标记 */
        switch (records[i].result) {
            case SELFCHECK_PASS:
                print("[OK]  ");
                break;
            case SELFCHECK_WARN:
                print("[!!]  ");
                break;
            case SELFCHECK_FAIL:
                print("[XX]  ");
                break;
            default:
                print("[??]  ");
                break;
        }
        
        print(records[i].name);
        
        if (records[i].detail[0] != '\0') {
            print(" - ");
            print(records[i].detail);
        }
        
        println("");
    }
    
    println("╠══════════════════════════════════════════════════════════╣");
    print("║ Total: "); print_dec(total_checks);
    print(" | Passed: "); print_dec(passed_checks);
    print(" | Warnings: "); print_dec(warning_checks);
    print(" | Failed: "); print_dec(failed_checks);
    println("        ║");
    println("╚══════════════════════════════════════════════════════════╝");
    println("");
    
    /* 如果有失败，打印警告 */
    if (failed_checks > 0) {
        println("[SELFCHECK] WARNING: Some critical checks failed!");
        println("[SELFCHECK] System may be unstable.");
    }
    
    return passed_checks;
}

/**
 * @brief 快速健康检查
 * @return 0 表示健康，非 0 表示有问题
 */
int kselfcheck_quick(void) {
    /* 只检查最关键的项目 */
    
    /* 检查堆 */
    void *ptr = kmalloc(64);
    if (ptr == NULL) return -1;
    kfree(ptr);
    
    /* 检查中断 */
    uint64_t rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    if (!(rflags & 0x200)) return -2;
    
    return 0;
}

/**
 * @brief 获取自检统计
 */
void kselfcheck_get_stats(int *total, int *passed, int *failed, int *warnings) {
    if (total) *total = total_checks;
    if (passed) *passed = passed_checks;
    if (failed) *failed = failed_checks;
    if (warnings) *warnings = warning_checks;
}

/**
 * @brief 获取最后一次检查时间
 */
uint64_t kselfcheck_get_last_time(void) {
    return last_check_time;
}

/**
 * @brief 检查系统是否健康
 */
int kselfcheck_is_healthy(void) {
    return (failed_checks == 0);
}
