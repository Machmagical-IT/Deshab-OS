#include "isr.h"
#include "console.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "mmap.h"
#include "cpu.h"
#include "limine.h"

#include "task.h"
#include "page_fault.h"

/* inb/outb 已在 cpu.h 中定义 */

/* 异常名称 (扩展到 32 个以覆盖所有异常向量) */
static const char *exception_names[] = {
    "Division Error",                  /* 0 */
    "Debug",                           /* 1 */
    "Non-Maskable Interrupt",          /* 2 */
    "Breakpoint",                      /* 3 */
    "Overflow",                        /* 4 */
    "BOUND Range Exceeded",            /* 5 */
    "Invalid Opcode",                  /* 6 */
    "Device Not Available",            /* 7 */
    "Double Fault",                    /* 8 */
    "Coprocessor Segment Overrun",     /* 9 */
    "Invalid TSS",                     /* 10 */
    "Segment Not Present",             /* 11 */
    "Stack-Segment Fault",             /* 12 */
    "General Protection",              /* 13 */
    "Page Fault",                      /* 14 */
    "Reserved",                        /* 15 */
    "x87 FPU Error",                   /* 16 */
    "Alignment Check",                 /* 17 */
    "Machine Check",                   /* 18 */
    "SIMD Exception",                  /* 19 */
    "Virtualization Exception",        /* 20 */
    "Control Protection Exception",    /* 21 */
    "Reserved",                        /* 22 */
    "Reserved",                        /* 23 */
    "Reserved",                        /* 24 */
    "Reserved",                        /* 25 */
    "Reserved",                        /* 26 */
    "Reserved",                        /* 27 */
    "Hypervisor Injection",            /* 28 */
    "VMM Communication Exception",     /* 29 */
    "Security Exception",              /* 30 */
    "Reserved"                         /* 31 */
};

#define EXCEPTION_NAMES_COUNT (sizeof(exception_names) / sizeof(exception_names[0]))

/* 安全获取异常名称 */
const char *get_exception_name_safe(uint8_t vector) {
    if (vector < EXCEPTION_NAMES_COUNT) {
        return exception_names[vector];
    }
    return "Unknown Exception";
}

/* 打印异常信息 */
static void print_exception(struct interrupt_frame *frame, const char *name) {
    serial_print("[EXCEPTION] ");
    serial_print(name);
    serial_print(" vector=");
    char vbuf[8]; int vp = 0; uint64_t vv = frame->int_no;
    if (vv == 0) vbuf[vp++] = '0';
    else { char t[8]; int tp = 0; while (vv > 0) { t[tp++] = '0' + vv % 10; vv /= 10; } while (tp > 0) vbuf[vp++] = t[--tp]; }
    vbuf[vp] = '\0';
    serial_print(vbuf);
    serial_print(" RIP=0x");
    char rbuf[20]; int rp = 0; uint64_t rv = frame->rip;
    for (int k = 15; k >= 0; k--) { rbuf[rp++] = "0123456789ABCDEF"[(rv >> (k*4)) & 0xF]; }
    rbuf[16] = '\0';
    serial_print(rbuf);
    
    /* 对于页错误，打印 CR2 */
    if (frame->int_no == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        serial_print(" CR2=0x");
        char cbuf[20]; int cp = 0;
        for (int k = 15; k >= 0; k--) { cbuf[cp++] = "0123456789ABCDEF"[(cr2 >> (k*4)) & 0xF]; }
        cbuf[16] = '\0';
        serial_print(cbuf);
    }
    serial_print("\n");
    
    println("");
    println("=== EXCEPTION ===");
    print("Name: ");
    println(name);
    print("Vector: ");
    print_hex(frame->int_no);
    println("");
    
    if (frame->err_code != 0) {
        print("Error Code: ");
        print_hex(frame->err_code);
        println("");
    }
    
    print("RIP: ");
    print_hex(frame->rip);
    println("");
    print("RSP: ");
    print_hex(frame->rsp);
    println("");
    print("RFLAGS: ");
    print_hex(frame->rflags);
    println("");
}

/* 除法错误 - 安全恢复：清零结果并返回0 */
void isr_handler_divide_error(struct interrupt_frame *frame) {
    print_exception(frame, exception_names[0]);
    /* 将除法结果设为0，防止后续使用垃圾值 */
    frame->rax = 0;
    frame->rdx = 0;
    /* 跳过 div/idiv 指令 (通常是3字节: 48 F7 /r) */
    frame->rip += 3;
}

/* 调试异常 */
void isr_handler_debug(struct interrupt_frame *frame) {
    print_exception(frame, exception_names[1]);
}

/* NMI */
void isr_handler_nmi(struct interrupt_frame *frame) {
    print_exception(frame, exception_names[2]);
    for (;;) { asm volatile("hlt"); }
}

/* 断点 */
void isr_handler_breakpoint(struct interrupt_frame *frame) {
    print_exception(frame, exception_names[3]);
}

/* 溢出 */
void isr_handler_overflow(struct interrupt_frame *frame) {
    print_exception(frame, exception_names[4]);
    for (;;) { asm volatile("hlt"); }
}

/* BOUND 范围超出 */
void isr_handler_bound_range(struct interrupt_frame *frame) {
    if (frame_from_user(frame)) {
        println("[EXCEPTION] User-space BOUND range exceeded, terminating process");
        process_exit(5);
        return;
    }
    print_exception(frame, exception_names[5]);
    for (;;) { asm volatile("hlt"); }
}

/* 无效操作码 */
void isr_handler_invalid_opcode(struct interrupt_frame *frame) {
    if (frame_from_user(frame)) {
        println("[EXCEPTION] User-space invalid opcode, terminating process");
        process_exit(4);
        return;
    }
    print_exception(frame, exception_names[6]);
    for (;;) { asm volatile("hlt"); }
}

/* 设备不可用 */
void isr_handler_device_not_available(struct interrupt_frame *frame) {
    if (frame_from_user(frame)) {
        println("[EXCEPTION] User-space device not available, terminating process");
        process_exit(8);
        return;
    }
    print_exception(frame, exception_names[7]);
    for (;;) { asm volatile("hlt"); }
}

/* 双重错误 */
void isr_handler_double_fault(struct interrupt_frame *frame) {
    /* 极早期调试输出 - 直接写入串口 */
    const char *df_msg = "[DF] Double Fault handler entered!\n";
    for (const char *p = df_msg; *p; p++) {
        while ((inb(0x3F8 + 5) & 0x20) == 0);
        outb(*p, 0x3F8);
    }
    
    print_exception(frame, exception_names[8]);
    for (;;) { asm volatile("hlt"); }
}

/* 无效 TSS */
void isr_handler_invalid_tss(struct interrupt_frame *frame) {
    print_exception(frame, exception_names[10]);
    for (;;) { asm volatile("hlt"); }
}

/* 段不存在 */
void isr_handler_segment_not_present(struct interrupt_frame *frame) {
    if (frame_from_user(frame)) {
        println("[EXCEPTION] User-space segment not present, terminating process");
        process_exit(11);
        return;
    }
    print_exception(frame, exception_names[11]);
    for (;;) { asm volatile("hlt"); }
}

/* 栈段错误 */
void isr_handler_stack_segment_fault(struct interrupt_frame *frame) {
    if (frame_from_user(frame)) {
        println("[EXCEPTION] User-space stack segment fault, terminating process");
        process_exit(12);
        return;
    }
    print_exception(frame, exception_names[12]);
    for (;;) { asm volatile("hlt"); }
}

/* 一般保护错误 */
void isr_handler_general_protection(struct interrupt_frame *frame) {
    /* 极早期调试输出 - 直接写入串口 */
    const char *gpf_msg = "[GPF] General Protection Fault handler entered!\n";
    for (const char *p = gpf_msg; *p; p++) {
        while ((inb(0x3F8 + 5) & 0x20) == 0);
        outb(*p, 0x3F8);
    }

    /* 区分用户态和内核态 GPF */
    if (frame_from_user(frame)) {
        println("[GPF] User-space General Protection Fault, terminating process");
        print_exception(frame, exception_names[13]);
        process_exit(13);
        return;
    }

    /* 内核态 GPF：致命错误，停机 */
    print_exception(frame, exception_names[13]);

    /* 尝试在屏幕上显示红色错误条，提供视觉反馈 */
    {
        extern volatile struct limine_framebuffer_request fb_request;
        struct limine_framebuffer_response *resp = fb_request.response;
        if (resp != NULL && resp->framebuffer_count > 0) {
            struct limine_framebuffer *lfb = resp->framebuffers[0];
            volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)lfb->address;
            uint32_t w = lfb->width;
            uint32_t h = lfb->height;
            uint32_t pitch = lfb->pitch / 4;
            /* 在屏幕顶部绘制红色错误条 */
            for (uint32_t y = 0; y < 40 && y < h; y++) {
                for (uint32_t x = 0; x < w; x++) {
                    fb[y * pitch + x] = 0xFFFF0000;
                }
            }
        }
    }

    for (;;) { asm volatile("hlt"); }
}

/* 检查地址是否在 HHDM 区域 */
static int is_hhdm_address(uint64_t addr) {
    /* HHDM 区域: PML4 index 256 (0xFFFF800000000000 - 0xFFFF80FFFFFFFFFF) */
    /* 这是 Limine 的标准 HHDM 映射区域 */
    uint64_t pml4_idx = (addr >> 39) & 0x1FF;
    return (pml4_idx == 256);
}

/* 尝试自动映射 HHDM 页面 */
static int try_map_hhdm_page(uint64_t virt_addr) {
    extern uint64_t hhdm_offset;
    extern struct address_space *vmm_get_kernel_space(void);
    extern int vmm_map_page(struct address_space *space, uint64_t virt, uint64_t phys, uint64_t flags);
    extern uint64_t pmm_get_total_memory(void);  /* 动态获取物理内存上限 */
    
    /* 检查是否是 HHDM 地址 */
    if (!is_hhdm_address(virt_addr)) {
        return 0;  /* 不是 HHDM 地址，无法自动映射 */
    }
    
    /* 检查 hhdm_offset 是否有效 */
    if (hhdm_offset == 0) {
        return 0;  /* HHDM 未初始化 */
    }
    
    /* 计算物理地址 */
    uint64_t phys_addr = virt_addr - hhdm_offset;
    
    /* 页对齐 */
    uint64_t page_virt = virt_addr & ~0xFFF;
    uint64_t page_phys = phys_addr & ~0xFFF;
    
    /* 安全检查：物理地址范围 */
    /* 动态获取物理内存上限，添加 256MB 余量用于 MMIO 区域 */
    uint64_t max_phys = pmm_get_total_memory();
    if (max_phys == 0) {
        max_phys = 0x100000000ULL;  /* 回退到 4GB */
    }
    max_phys += 0x10000000ULL;  /* 添加 256MB 余量 */
    
    if (phys_addr >= max_phys) {
        /* 超出物理内存范围，可能是无效访问 */
        return 0;
    }
    
    /* 获取内核地址空间 */
    struct address_space *space = vmm_get_kernel_space();
    if (space == NULL) {
        return 0;
    }
    
    /* 映射页面 */
    /* 对于保留区域（BIOS ROM, ACPI 等），使用缓存禁用 */
    /* 对于常规内存，使用普通缓存 */
    uint64_t flags;
    if (phys_addr >= 0xE0000 && phys_addr < 0x100000) {
        /* BIOS ROM 区域：禁用缓存 */
        flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITETHROUGH | VMM_FLAG_CACHE_DISABLE;
    } else if (phys_addr < 0x100000) {
        /* 低端内存区域：可能也是保留的 */
        flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITETHROUGH | VMM_FLAG_CACHE_DISABLE;
    } else {
        /* 常规内存：允许缓存，可读写 */
        flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    }
    
    int result = vmm_map_page(space, page_virt, page_phys, flags);
    return (result == 0) ? 1 : 0;
}

/* 尝试映射用户空间页面 */

/* 重复失败地址跟踪（防止无限循环） */
#define MAX_TRACKED_ADDRESSES 32
#define MAX_FAILURE_COUNT 3

static struct {
    uint64_t addr;
    uint32_t count;
    uint32_t pad;
} failed_address_cache[MAX_TRACKED_ADDRESSES];

static int failed_address_index = 0;
static spinlock_t failed_addr_lock = SPINLOCK_INIT;

/* 检查地址是否频繁失败 */
static int is_frequently_failing(uint64_t addr) {
    uint64_t page_addr = addr & ~0xFFF;
    for (int i = 0; i < MAX_TRACKED_ADDRESSES; i++) {
        if (failed_address_cache[i].addr == page_addr && 
            failed_address_cache[i].count >= MAX_FAILURE_COUNT) {
            return 1;
        }
    }
    return 0;
}

/* 记录失败的地址 */
static void record_failed_address(uint64_t addr) {
    uint64_t page_addr = addr & ~0xFFF;
    
    spin_lock(&failed_addr_lock);
    
    /* 先检查是否已存在 */
    for (int i = 0; i < MAX_TRACKED_ADDRESSES; i++) {
        if (failed_address_cache[i].addr == page_addr) {
            failed_address_cache[i].count++;
            spin_unlock(&failed_addr_lock);
            return;
        }
    }
    
    /* 添加新条目 */
    int idx = failed_address_index;
    failed_address_cache[idx].addr = page_addr;
    failed_address_cache[idx].count = 1;
    failed_address_index = (failed_address_index + 1) % MAX_TRACKED_ADDRESSES;
    
    spin_unlock(&failed_addr_lock);
}

static int try_map_user_page(uint64_t virt_addr) {
    struct process *current_proc = get_current_task();  /* 使用函数而不是外部变量 */
    
    /* 检查是否是用户空间地址 */
    if (virt_addr >= 0x800000000000ULL) {
        return 0;  /* 不是用户空间地址 */
    }
    
    /* 额外的安全检查：确保地址不是超低地址（如NULL指针） */
    if (virt_addr < 0x1000) {
        return 0;  /* 超低地址，很可能是空指针解引用，不进行自动映射 */
    }
    
    /* 检查是否是频繁失败的地址（防止无限循环） */
    if (is_frequently_failing(virt_addr)) {
        print("[PF] Address 0x");
        print_hex(virt_addr);
        println(" is repeatedly failing, skipping auto-mapping");
        return 0;
    }
    
    /* 获取当前任务的地址空间 */
    if (current_proc == NULL) {
        return 0;  /* 没有当前任务 */
    }
    
    struct address_space *task_space = current_proc->space;
    if (task_space == NULL || task_space == vmm_get_kernel_space()) {
        return 0;  /* 任务没有有效的用户地址空间 */
    }
    
    /* 检查这个地址是否应该被映射（例如，是否在用户堆栈或堆区域内） */
    uint64_t page_virt = virt_addr & ~0xFFF;
    
    /* 验证地址是否在有效的 VMA 区域内 */
    int in_valid_vma = 0;
    if (task_space->mmap_mgr != NULL) {
        struct vm_area_struct *vma = vma_find(task_space, virt_addr);
        if (vma != NULL) {
            in_valid_vma = 1;
            print("[PF] Address 0x");
            print_hex(virt_addr);
            print(" found in VMA [0x");
            print_hex(vma->vm_start);
            print("-0x");
            print_hex(vma->vm_end);
            println("]");
        }
    }
    
    /* 只在有效 VMA 区域或已知的安全区域（堆栈/堆）进行自动映射 */
    if (in_valid_vma ||
        (virt_addr >= 0x700000000000ULL && virt_addr < 0x800000000000ULL) ||
        (virt_addr >= 0x0000000000400000ULL && virt_addr < 0x0000010000000000ULL)) {
        /* 可能是堆栈或堆区域，尝试分配一个页面 */
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            record_failed_address(virt_addr);
            return 0;  /* 无法分配物理页 */
        }
        
        /* 清零物理页 */
        void *virt_page = phys_to_virt(phys);
        if (virt_page == NULL) {
            /* phys_to_virt 失败，释放物理页并返回失败 */
            pmm_free_page(phys);
            record_failed_address(virt_addr);
            return 0;
        }
        memset(virt_page, 0, PAGE_SIZE);
        
        /* 映射到任务的地址空间 */
        int result = vmm_map_page(task_space, page_virt, phys, 
                                  VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER);
        if (result != 0) {
            pmm_free_page(phys);  /* 分配失败，释放物理页 */
            record_failed_address(virt_addr);
            return 0;
        }
        
        /* 更新 CR3 以使用新映射 */
        if (task_space == vmm_get_current_space()) {
            asm volatile("mov %0, %%cr3" :: "r"(task_space->cr3));
        }
        
        return 1;  /* 映射成功 */
    }
    
    /* 地址不在有效 VMA 区域内，记录失败并拒绝 */
    record_failed_address(virt_addr);
    print("[PF] Address 0x");
    print_hex(virt_addr);
    println(" not in valid VMA, rejecting auto-mapping");
    return 0;
}

/* 页错误 */
void isr_handler_page_fault(struct interrupt_frame *frame) {
    /* 极早期调试输出 - 直接写入串口 */
    const char *pf_msg = "[PF] Page fault handler entered!\n";
    for (const char *p = pf_msg; *p; p++) {
        while ((inb(0x3F8 + 5) & 0x20) == 0);
        outb(*p, 0x3F8);
    }
    
    uint64_t cr2;
    uint64_t cr3;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    
    /* 输出 CR2 值 */
    const char hex[] = "0123456789ABCDEF";
    char cr2_buf[] = "[PF] CR2=0x0000000000000000\n";
    for (int i = 0; i < 16; i++) {
        cr2_buf[9 + i] = hex[(cr2 >> (60 - i * 4)) & 0xF];
    }
    for (const char *p = cr2_buf; *p; p++) {
        while ((inb(0x3F8 + 5) & 0x20) == 0);
        outb(*p, 0x3F8);
    }
    
    /* 使用新的页错误处理系统 */
    extern pf_result_t handle_page_fault(struct interrupt_frame *frame, uint64_t fault_addr);
    pf_result_t result = handle_page_fault(frame, cr2);
    
    /* 如果页错误被成功处理，直接返回 */
    if (result == PF_HANDLED_SUCCESS) {
        return;
    }
    
    /* 如果指令被跳过，继续执行 */
    if (result == PF_SKIPPED) {
        console_set_color(CONSOLE_SUCCESS_GREEN, CONSOLE_BG_DARK);
        println("[PF] Fault handled by instruction skip, resuming execution...");
        return;
    }
    
    /* 如果是致命错误,立即停止以防止无限循环 */
    if (result == PF_HANDLED_FATAL) {
        /* 清屏并设置深色背景 */
        console_set_color(CONSOLE_TEXT_WHITE, CONSOLE_BG_DARK);
        
        println("");
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色标题 */
        println("╔══════════════════════════════════════════════════════════╗");
        println("║              !!! FATAL PAGE FAULT EXCEPTION !!!          ║");
        println("╚══════════════════════════════════════════════════════════╝");
        println("");
        
        /* 跳转到错误显示部分，避免重复处理 */
        goto show_error;
    }
    
    /* 获取当前地址空间进行额外处理 */
    extern struct address_space *vmm_get_current_space(void);
    struct address_space *current_space = vmm_get_current_space();

    /* ===== 尝试额外的页错误处理策略 ===== */

    /* 内核态页错误：先检查异常表（copy_to_user/copy_from_user 修复点），
     * 如果不在异常表中，再报告致命错误 */
    if (!frame_from_user(frame)) {
        /* 搜索异常表 - 用于 copy_to_user/copy_from_user 的页错误恢复 */
        extern const uint64_t __ex_table_start[];
        extern const uint64_t __ex_table_end[];
        uint64_t fault_rip = frame->rip;
        for (const uint64_t *entry = __ex_table_start; entry < __ex_table_end; entry += 2) {
            if (entry[0] == fault_rip) {
                /* 找到匹配的异常表项，跳转到修复代码 */
                frame->rip = entry[1];
                return;
            }
        }
        /* 不在异常表中，报告致命错误 */
        goto show_error;
    }

    /* ===== 尝试 HHDM 自动映射 ===== */
    if (!(frame->err_code & 1) && is_hhdm_address(cr2)) {
        if (try_map_hhdm_page(cr2)) {
            /* 映射成功，返回继续执行 */
            return;
        }
    }
    
    /* ===== 尝试用户空间自动映射 ===== */
    /* Handle both user mode and kernel mode access to user space addresses */
    /* This can happen when kernel accesses user space during system calls */
    if (!(frame->err_code & 1) && cr2 < 0x800000000000ULL) {
        /* Check for null pointer or very low addresses to avoid infinite loops */
        if (cr2 < 0x1000) {
            /* This is likely a null pointer dereference, treat as fatal */
            console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);
            print("[FATAL] Null pointer dereference at address: 0x");
            print_hex(cr2);
            println("");
            goto show_error;
        }
        
        /* User space address access (could be from kernel or user mode) */
        if (try_map_user_page(cr2)) {
            /* 映射成功，返回继续执行 */
            return;
        }
    }
    
    /* ===== 检查 VMA 并处理内存映射错误 ===== */
    if (!(frame->err_code & 1) && current_space != NULL && current_space->mmap_mgr != NULL) {
        /* 页不存在错误，尝试查找 VMA 并处理 */
        struct vm_area_struct *vma = vma_find(current_space, cr2);
        if (vma != NULL) {
            /* 找到对应的 VMA，尝试处理页错误 */
            if (mmap_handle_fault(vma, cr2, frame->err_code) == 0) {
                /* VMA 处理成功，返回继续执行 */
                return;
            }
        }
    }
    
    /* ===== 按需分页处理：尝试为用户空间创建页表项 ===== */
    if (!(frame->err_code & 1) && cr2 < 0x800000000000ULL && current_space != NULL) {
        /* 用户空间地址，尝试按需分页 */
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (frame->err_code & (1 << 1)) {  /* 写操作 */
            flags |= VMM_FLAG_WRITABLE;
        }
        
        if (vmm_create_pte_if_not_present(current_space, cr2, flags) == 0) {
            /* 按需分页成功，返回继续执行 */
            return;
        }
    }
    
    /* ===== 尝试跳过指令 (内核访问用户空间) ===== */
    /* 这是内核尝试访问用户空间地址失败后的最后一道防线 */
    extern int pf_can_skip_fault(uint64_t fault_addr, uint64_t rip, uint64_t error_code);
    extern int pf_skip_instruction(struct interrupt_frame *frame);
    
    if (pf_can_skip_fault(cr2, frame->rip, frame->err_code)) {
        console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
        print("[PF] Attempting to skip faulting instruction at 0x");
        print_hex(frame->rip);
        print(" for address 0x");
        print_hex(cr2);
        println("");
        
        if (pf_skip_instruction(frame) == 0) {
            console_set_color(CONSOLE_SUCCESS_GREEN, CONSOLE_BG_DARK);
            println("[PF] Instruction skipped successfully, continuing execution...");
            return;  /* 成功跳过，继续执行 */
        }
        
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);
        println("[PF] Failed to skip instruction");
    }
    
    /* 如果所有尝试都失败，显示错误信息 */
    console_set_color(CONSOLE_TEXT_WHITE, CONSOLE_BG_DARK);
    println("");
    console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色标题 */
    println("╔══════════════════════════════════════════════════════════╗");
    println("║              !!! PAGE FAULT EXCEPTION !!!                ║");
    println("╚══════════════════════════════════════════════════════════╝");
    println("");

show_error:
    /* 异常基本信息 */
    console_set_color(0xF0F6FC, 0x0D1117);  /* 白色 */
    print("Name: ");
    console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色 */
    println(exception_names[14]);
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("Vector: ");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);  /* 黄色 */
    print_hex(frame->int_no);
    println("");
    
    if (frame->err_code != 0) {
        console_set_color(0xF0F6FC, 0x0D1117);
        print("Error Code: ");
        console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
        print_hex(frame->err_code);
        println("");
    }
    
    /* 寄存器状态 - 关键信息用红色 */
    println("");
    console_set_color(CONSOLE_INFO_CYAN, CONSOLE_BG_DARK);  /* 青色标题 */
    println("=== Register State ===");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("RIP: ");
    console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色关键值 */
    print_hex(frame->rip);
    
    /* RIP 分析 - 立即显示警告 */
    if (frame->rip < 0xFFFFFFFF80000000ULL) {
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色警告 */
        print(" <-- !!! INVALID: Not in Higher Half Kernel !!!");
    }
    println("");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("RSP: ");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
    print_hex(frame->rsp);
    println("");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("RFLAGS: ");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
    print_hex(frame->rflags);
    println("");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("CR2 (Faulting Address): ");
    console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色关键值 */
    print_hex(cr2);
    println("");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("CR3 (Page Table Base): ");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
    print_hex(cr3);
    println("");
    
    /* 错误码分析 */
    println("");
    console_set_color(CONSOLE_INFO_CYAN, CONSOLE_BG_DARK);
    println("=== Error Code Analysis ===");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("P (Present):      ");
    if (frame->err_code & 1) {
        console_set_color(CONSOLE_SUCCESS_GREEN, CONSOLE_BG_DARK);  /* 绿色 */
        println("1 (Page Present)");
    } else {
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色 */
        println("0 (Page Not Present) <-- !!! FAULT CAUSE !!!");
    }
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("W/R (Write):      ");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
    print(frame->err_code & 2 ? "1 (Write)" : "0 (Read)");
    println("");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("U/S (User):       ");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
    println(frame->err_code & 4 ? "1 (User Mode)" : "0 (Supervisor/Kernel)");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("I/D (Execute):    ");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
    println(frame->err_code & 16 ? "1 (Instruction Fetch)" : "0 (Data Access)");
    
    /* 故障地址分析 */
    println("");
    console_set_color(CONSOLE_INFO_CYAN, CONSOLE_BG_DARK);
    println("=== Fault Address Analysis ===");
    
    console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色地址 */
    print("Address: ");
    print_hex(cr2);
    println("");
    
    /* 判断地址类型 */
    console_set_color(0xF0F6FC, 0x0D1117);
    print("Type: ");
    if (cr2 >= 0xFFFFFFFF80000000ULL) {
        console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
        print("Higher Half Kernel Address");
    } else if (cr2 >= 0xFFFF800000000000ULL) {
        console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
        print("Kernel Space Address");
    } else if (cr2 == 0) {
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色 */
        print("!!! NULL POINTER DEREFERENCE !!!");
    } else if (cr2 < 0x1000) {
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色 */
        print("!!! VERY LOW ADDRESS (NULL+offset) !!!");
    } else if (cr2 >= 0x100000ULL) {
        console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
        print("User Space Address");
    } else {
        console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
        print("Low Memory Address");
    }
    println("");
    
    /* 页表索引 */
    println("");
    console_set_color(CONSOLE_INFO_CYAN, CONSOLE_BG_DARK);
    println("=== Page Table Indices ===");
    
    uint64_t pml4_idx = (cr2 >> 39) & 0x1FF;
    uint64_t pdpt_idx = (cr2 >> 30) & 0x1FF;
    uint64_t pd_idx = (cr2 >> 21) & 0x1FF;
    uint64_t pt_idx = (cr2 >> 12) & 0x1FF;
    uint64_t offset = cr2 & 0xFFF;
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("PML4 Index: ");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
    print_dec(pml4_idx);
    print(" (0x");
    print_hex(pml4_idx);
    println(")");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("PDPT Index: ");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
    print_dec(pdpt_idx);
    print(" (0x");
    print_hex(pdpt_idx);
    println(")");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("PD Index:   ");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
    print_dec(pd_idx);
    print(" (0x");
    print_hex(pd_idx);
    println(")");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("PT Index:   ");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
    print_dec(pt_idx);
    print(" (0x");
    print_hex(pt_idx);
    println(")");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("Page Offset: 0x");
    console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
    print_hex(offset);
    println("");
    
    /* RIP 详细分析 */
    println("");
    console_set_color(CONSOLE_INFO_CYAN, CONSOLE_BG_DARK);
    println("=== RIP Analysis ===");
    
    console_set_color(0xF0F6FC, 0x0D1117);
    print("Fault occurred at: ");
    console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色 */
    print_hex(frame->rip);
    println("");
    
    if (frame->rip >= 0xFFFFFFFF80000000ULL) {
        console_set_color(CONSOLE_SUCCESS_GREEN, CONSOLE_BG_DARK);  /* 绿色 */
        println("RIP is in Higher Half Kernel (valid)");
    } else if (frame->rip == 0) {
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色 */
        println("!!! RIP is NULL - Function pointer corrupted !!!");
    } else if (frame->rip < 0x10000) {
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色 */
        println("!!! RIP is in low memory - Possible jump to invalid address !!!");
    } else {
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色 */
        println("!!! RIP is NOT in Higher Half - Memory corruption or bad pointer !!!");
    }
    
    /* 转储栈内容 */
    println("");
    console_set_color(CONSOLE_INFO_CYAN, CONSOLE_BG_DARK);
    println("=== Stack Dump (16 qwords) ===");
    
    /* 安全验证栈指针 */
    uint64_t *rsp_ptr = (uint64_t *)frame->rsp;
    int stack_valid = 1;
    
    /* 检查栈指针是否在合理范围内 */
    if (frame->rsp == 0) {
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);
        println("  [Stack dump skipped: RSP is NULL]");
        stack_valid = 0;
    } else if (frame->rsp < 0x1000) {
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);
        println("  [Stack dump skipped: RSP too low]");
        stack_valid = 0;
    } else if (frame->rsp >= 0xFFFFFFFF80000000ULL) {
        /* Higher Half 内核栈 - 有效 */
        stack_valid = 1;
    } else if (frame->rsp >= 0xFFFF800000000000ULL) {
        /* 内核空间 - 有效 */
        stack_valid = 1;
    } else if (frame->rsp >= 0x100000ULL) {
        /* 用户空间栈 - 可能有效但需要小心 */
        console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
        println("  [RSP in user space - attempting safe read]");
    } else {
        console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);
        println("  [Stack dump skipped: RSP in invalid range]");
        stack_valid = 0;
    }
    
    if (stack_valid) {
        for (int i = 0; i < 16; i++) {
            console_set_color(0xF0F6FC, 0x0D1117);
            print("RSP+");
            print_dec(i * 8);
            print(": ");
            
            /* 安全读取内存 - 使用异常处理机制 */
            uint64_t val = 0;
            int read_ok = 0;
            
            /* 使用内联汇编尝试读取，捕获可能的异常 */
            __asm__ volatile (
                "1: movq (%[ptr]), %[val]\n\t"
                "movl $1, %[ok]\n\t"
                "jmp 2f\n\t"
                ".section .text.fixup, \"ax\"\n\t"
                "3: movl $0, %[ok]\n\t"
                "jmp 2f\n\t"
                ".previous\n\t"
                ".section __ex_table, \"a\"\n\t"
                ".quad 1b, 3b\n\t"
                ".previous\n\t"
                "2:"
                : [val] "=r" (val), [ok] "=r" (read_ok)
                : [ptr] "r" (&rsp_ptr[i])
                : "memory"
            );
            
            if (read_ok) {
                console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK);
                print_hex(val);
                
                /* 标记可疑值 */
                if (val == 0) {
                    console_set_color(CONSOLE_TEXT_GRAY, CONSOLE_BG_DARK);
                    print(" (zero)");
                } else if (val < 0x1000) {
                    console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);
                    print(" !!! LOW VALUE");
                } else if (val >= 0xFFFFFFFF80000000ULL) {
                    console_set_color(CONSOLE_SUCCESS_GREEN, CONSOLE_BG_DARK);
                    print(" (kernel)");
                }
            } else {
                console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);
                print("[UNREADABLE]");
            }
            println("");
        }
    }
    
    /* 系统停机提示 */
    println("");
    console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK);  /* 红色 */
    println("=== SYSTEM HALTED ===");
    console_set_color(0xF0F6FC, 0x0D1117);
    println("Please check the faulting address and page table configuration.");
    println("");
    console_set_color(CONSOLE_INFO_CYAN, CONSOLE_BG_DARK);
    println("Possible causes:");
    console_set_color(CONSOLE_TEXT_GRAY, CONSOLE_BG_DARK);
    println("  - Invalid function pointer");
    println("  - Stack corruption");
    println("  - Missing page table mapping");
    println("  - Structure offset mismatch between C and assembly");
    
    for (;;) { asm volatile("hlt"); }
}

/* x87 FPU 错误 */
void isr_handler_fpu_error(struct interrupt_frame *frame) {
    if (frame_from_user(frame)) {
        println("[EXCEPTION] User-space x87 FPU error, terminating process");
        process_exit(8);
        return;
    }
    print_exception(frame, exception_names[16]);
    for (;;) { asm volatile("hlt"); }
}

/* 对齐检查 */
void isr_handler_alignment_check(struct interrupt_frame *frame) {
    if (frame_from_user(frame)) {
        println("[EXCEPTION] User-space alignment check, terminating process");
        process_exit(7);
        return;
    }
    print_exception(frame, exception_names[17]);
    for (;;) { asm volatile("hlt"); }
}

/* 机器检查 */
void isr_handler_machine_check(struct interrupt_frame *frame) {
    /* 机器检查总是致命的，不区分用户/内核 */
    print_exception(frame, exception_names[18]);
    for (;;) { asm volatile("hlt"); }
}

/* SIMD 异常 */
void isr_handler_simd_exception(struct interrupt_frame *frame) {
    if (frame_from_user(frame)) {
        println("[EXCEPTION] User-space SIMD exception, terminating process");
        process_exit(8);
        return;
    }
    print_exception(frame, exception_names[19]);
    for (;;) { asm volatile("hlt"); }
}

/* ========== 中断处理增强功能 ========== */

/* 中断统计 */
struct isr_stats {
    uint64_t counts[IDT_ENTRIES];       /* 每个中断的调用次数 */
    uint64_t total_interrupts;          /* 总中断数 */
    uint64_t spurious_count;            /* 伪中断数 */
    uint64_t unhandled_count;           /* 未处理中断数 */
    uint64_t max_latency;               /* 最大延迟 (ticks) */
    uint64_t total_latency;             /* 总延迟 */
    uint64_t overflow_count;            /* 中断溢出计数 */
};

static struct isr_stats isr_stats;

/* 中断嵌套深度 */
static volatile int isr_nesting_depth = 0;
static volatile int isr_max_nesting = 0;

/* 中断栈检查 */
#define ISR_STACK_MAGIC    0xDEADBEEF12345678ULL
static uint64_t isr_stack_check = ISR_STACK_MAGIC;

/* 初始化中断统计 */
void isr_stats_init(void) {
    for (int i = 0; i < IDT_ENTRIES; i++) {
        isr_stats.counts[i] = 0;
    }
    isr_stats.total_interrupts = 0;
    isr_stats.spurious_count = 0;
    isr_stats.unhandled_count = 0;
    isr_stats.max_latency = 0;
    isr_stats.total_latency = 0;
    isr_stats.overflow_count = 0;
    isr_nesting_depth = 0;
    isr_max_nesting = 0;
}

/* 中断入口钩子 */
void isr_entry_hook(uint8_t vector) {
    isr_nesting_depth++;
    if (isr_nesting_depth > isr_max_nesting) {
        isr_max_nesting = isr_nesting_depth;
    }
    
    /* 检查栈完整性 */
    if (isr_stack_check != ISR_STACK_MAGIC) {
        println("[ISR] Stack corruption detected!");
        isr_stack_check = ISR_STACK_MAGIC;
    }
}

/* 中断出口钩子 */
void isr_exit_hook(uint8_t vector) {
    isr_nesting_depth--;
    isr_stats.counts[vector]++;
    isr_stats.total_interrupts++;
}

/* 获取中断统计 */
void isr_get_stats(struct isr_stats *stats) {
    if (stats != NULL) {
        *stats = isr_stats;
    }
}

/* 打印中断统计 */
void isr_print_stats(void) {
    println("=== Interrupt Statistics ===");
    print("Total interrupts: ");
    print_dec(isr_stats.total_interrupts);
    println("");
    print("Spurious: ");
    print_dec(isr_stats.spurious_count);
    println("");
    print("Unhandled: ");
    print_dec(isr_stats.unhandled_count);
    println("");
    print("Max nesting: ");
    print_dec(isr_max_nesting);
    println("");
    
    /* 打印前10个最频繁的中断 */
    println("Top interrupts:");
    for (int j = 0; j < 10; j++) {
        uint64_t max = 0;
        int max_idx = -1;
        
        for (int i = 0; i < IDT_ENTRIES; i++) {
            if (isr_stats.counts[i] > max) {
                max = isr_stats.counts[i];
                max_idx = i;
            }
        }
        
        if (max_idx >= 0 && max > 0) {
            print("  Vector ");
            print_dec(max_idx);
            print(": ");
            print_dec(max);
            println("");
            isr_stats.counts[max_idx] = 0;  /* 临时清零用于排序 */
        }
    }
}

/* 快速中断分发器 */
typedef void (*fast_isr_t)(void);

/* 预注册的快速处理程序 */
static fast_isr_t fast_handlers[256];

/* 注册快速中断处理程序 */
int isr_register_fast(uint8_t vector, fast_isr_t handler) {
    if (vector >= IDT_ENTRIES) return -1;
    fast_handlers[vector] = handler;
    return 0;
}

/* 快速中断入口 (无参数传递) */
void fast_isr_dispatch(uint8_t vector) {
    if (fast_handlers[vector] != NULL) {
        fast_handlers[vector]();
    }
}

/* ========== 异常恢复机制 ========== */

/* 可恢复异常表 */
struct recoverable_exception {
    uint8_t vector;
    int (*recover)(struct interrupt_frame *frame);
    const char *description;
};

static struct recoverable_exception recover_table[16];
static int recover_count = 0;

/* 注册可恢复异常 */
int isr_register_recovery(uint8_t vector, int (*recover)(struct interrupt_frame *frame),
                          const char *desc) {
    if (recover_count >= 16) return -1;
    
    recover_table[recover_count].vector = vector;
    recover_table[recover_count].recover = recover;
    recover_table[recover_count].description = desc;
    recover_count++;
    
    return 0;
}

/* 尝试恢复异常 */
int isr_try_recover(struct interrupt_frame *frame) {
    for (int i = 0; i < recover_count; i++) {
        if (recover_table[i].vector == frame->int_no) {
            if (recover_table[i].recover != NULL) {
                return recover_table[i].recover(frame);
            }
        }
    }
    return -1;  /* 无法恢复 */
}

/* ========== 中断优先级 ========== */

/* 中断优先级表 (数值越高优先级越高) */
static const int isr_priority[IDT_ENTRIES] = {
    [0]  = 10,  /* #DE - 高优先级 */
    [1]  = 5,   /* #DB - 调试 */
    [2]  = 20,  /* NMI - 最高 */
    [3]  = 3,   /* #BP - 断点 */
    [6]  = 10,  /* #UD - 非法指令 */
    [8]  = 20,  /* #DF - 双重故障 */
    [13] = 15,  /* #GP - 一般保护 */
    [14] = 15,  /* #PF - 页故障 */
    [18] = 20,  /* #MC - 机器检查 */
    /* 其他默认为 0 */
};

/* 获取中断优先级 */
int isr_get_priority(uint8_t vector) {
    if (vector >= IDT_ENTRIES) return 0;
    return isr_priority[vector];
}

/* ========== 中断风暴检测 ========== */

#define STORM_THRESHOLD     10000   /* 每秒中断阈值 */
#define STORM_WINDOW        1000000 /* 检测窗口 (微秒) */

static uint64_t storm_check_time = 0;
static uint64_t storm_interrupt_count = 0;

/* 检测中断风暴 */
int isr_detect_storm(void) {
    extern uint64_t get_tick_count(void);
    uint64_t now = get_tick_count();
    
    storm_interrupt_count++;
    
    /* 每隔检测窗口检查一次 */
    if (now - storm_check_time > STORM_WINDOW) {
        if (storm_interrupt_count > STORM_THRESHOLD) {
            println("[ISR] Interrupt storm detected!");
            print("  Interrupts in window: ");
            print_dec(storm_interrupt_count);
            println("");
            
            /* 重置计数 */
            storm_interrupt_count = 0;
            storm_check_time = now;
            return 1;
        }
        
        storm_interrupt_count = 0;
        storm_check_time = now;
    }
    
    return 0;
}

/* ========== 中断日志 ========== */

#define ISR_LOG_SIZE    256

struct isr_log_entry {
    uint64_t timestamp;
    uint8_t vector;
    uint8_t flags;
    uint16_t cpu;
    uint64_t rip;
};

static struct isr_log_entry isr_log[ISR_LOG_SIZE];
static volatile uint32_t isr_log_head = 0;
static volatile int isr_log_lock = 0;

/* 记录中断日志 */
void isr_log_entry(uint8_t vector, uint64_t rip) {
    while (__sync_lock_test_and_set(&isr_log_lock, 1)) {
        asm volatile("pause");
    }
    
    uint32_t idx = isr_log_head;
    isr_log[idx].timestamp = 0;  /* get_tick_count() */
    isr_log[idx].vector = vector;
    isr_log[idx].flags = 0;
    isr_log[idx].cpu = 0;  /* smp_processor_id() */
    isr_log[idx].rip = rip;
    
    isr_log_head = (isr_log_head + 1) % ISR_LOG_SIZE;
    
    __sync_lock_release(&isr_log_lock);
}

/* 获取中断日志 */
void isr_get_log(struct isr_log_entry *entries, uint32_t *count) {
    if (entries != NULL && count != NULL) {
        *count = ISR_LOG_SIZE;
        for (uint32_t i = 0; i < ISR_LOG_SIZE; i++) {
            entries[i] = isr_log[i];
        }
    }
}

/* ========== 系统健康监控 ========== */

/* 健康状态 */
typedef enum {
    HEALTH_OK = 0,
    HEALTH_WARNING,
    HEALTH_CRITICAL,
    HEALTH_FAILURE
} health_status_t;

/* 健康检查项 */
struct health_check {
    const char *name;
    health_status_t status;
    uint64_t last_check;
    uint64_t fail_count;
    uint64_t recover_count;
    int (*check_fn)(void);
};

/* 系统健康统计 */
struct system_health {
    health_status_t overall_status;
    uint64_t uptime_ns;
    uint64_t last_boot;
    uint32_t cpu_errors;
    uint32_t mem_errors;
    uint32_t io_errors;
    uint32_t net_errors;
    uint64_t total_interrupts;
    uint64_t total_page_faults;
    uint64_t total_context_switches;
};

static struct system_health sys_health;

/* 健康检查项数组 */
#define HEALTH_CHECK_MAX    16
static struct health_check health_checks[HEALTH_CHECK_MAX];
static int health_check_count = 0;

/* 初始化健康监控 */
void health_monitor_init(void) {
    sys_health.overall_status = HEALTH_OK;
    sys_health.uptime_ns = 0;
    sys_health.last_boot = 0;
    sys_health.cpu_errors = 0;
    sys_health.mem_errors = 0;
    sys_health.io_errors = 0;
    sys_health.net_errors = 0;
    sys_health.total_interrupts = 0;
    sys_health.total_page_faults = 0;
    sys_health.total_context_switches = 0;
    
    health_check_count = 0;
    for (int i = 0; i < HEALTH_CHECK_MAX; i++) {
        health_checks[i].name = NULL;
        health_checks[i].status = HEALTH_OK;
        health_checks[i].last_check = 0;
        health_checks[i].fail_count = 0;
        health_checks[i].recover_count = 0;
        health_checks[i].check_fn = NULL;
    }
}

/* 注册健康检查项 */
int health_register_check(const char *name, int (*check_fn)(void)) {
    if (health_check_count >= HEALTH_CHECK_MAX) return -1;
    if (name == NULL || check_fn == NULL) return -1;
    
    struct health_check *hc = &health_checks[health_check_count];
    hc->name = name;
    hc->check_fn = check_fn;
    hc->status = HEALTH_OK;
    hc->last_check = 0;
    hc->fail_count = 0;
    hc->recover_count = 0;
    
    health_check_count++;
    return 0;
}

/* 执行健康检查 */
void health_run_checks(void) {
    health_status_t worst_status = HEALTH_OK;
    
    for (int i = 0; i < health_check_count; i++) {
        struct health_check *hc = &health_checks[i];
        if (hc->check_fn == NULL) continue;
        
        hc->last_check = get_tick_count();
        int result = hc->check_fn();
        
        if (result == 0) {
            if (hc->status != HEALTH_OK) {
                hc->recover_count++;
            }
            hc->status = HEALTH_OK;
        } else if (result == 1) {
            hc->status = HEALTH_WARNING;
        } else if (result == 2) {
            hc->status = HEALTH_CRITICAL;
            hc->fail_count++;
        } else {
            hc->status = HEALTH_FAILURE;
            hc->fail_count++;
        }
        
        if (hc->status > worst_status) {
            worst_status = hc->status;
        }
    }
    
    sys_health.overall_status = worst_status;
}

/* 记录错误 */
void health_record_error(int type) {
    switch (type) {
        case 0: sys_health.cpu_errors++; break;
        case 1: sys_health.mem_errors++; break;
        case 2: sys_health.io_errors++; break;
        case 3: sys_health.net_errors++; break;
    }
}

/* 记录中断 */
void health_record_interrupt(void) {
    sys_health.total_interrupts++;
}

/* 记录页错误 */
void health_record_page_fault(void) {
    sys_health.total_page_faults++;
}

/* 记录上下文切换 */
void health_record_context_switch(void) {
    sys_health.total_context_switches++;
}

/* 获取健康状态 */
void health_get_status(struct system_health *status) {
    if (status != NULL) {
        *status = sys_health;
    }
}

/* 健康检查: CPU */
int health_check_cpu(void) {
    /* 检查 CPU 错误计数 */
    if (sys_health.cpu_errors > 100) {
        return 2;  /* CRITICAL */
    } else if (sys_health.cpu_errors > 10) {
        return 1;  /* WARNING */
    }
    return 0;  /* OK */
}

/* 健康检查: 内存 */
int health_check_memory(void) {
    /* 检查内存错误计数 */
    if (sys_health.mem_errors > 50) {
        return 2;
    } else if (sys_health.mem_errors > 5) {
        return 1;
    }
    return 0;
}

/* 健康检查: I/O */
int health_check_io(void) {
    if (sys_health.io_errors > 100) {
        return 2;
    } else if (sys_health.io_errors > 10) {
        return 1;
    }
    return 0;
}

/* 打印健康报告 */
void health_print_report_full(void) {
    println("╔══════════════════════════════════════════╗");
    println("║         System Health Report             ║");
    println("╚══════════════════════════════════════════╝");
    
    const char *status_names[] = {"OK", "WARNING", "CRITICAL", "FAILURE"};
    
    print("Overall Status: ");
    println(status_names[sys_health.overall_status]);
    
    println("");
    println("Error Counts:");
    print("  CPU Errors:   "); print_dec(sys_health.cpu_errors); println("");
    print("  Mem Errors:   "); print_dec(sys_health.mem_errors); println("");
    print("  I/O Errors:   "); print_dec(sys_health.io_errors); println("");
    print("  Net Errors:   "); print_dec(sys_health.net_errors); println("");
    
    println("");
    println("Statistics:");
    print("  Total Interrupts:     "); print_dec(sys_health.total_interrupts); println("");
    print("  Total Page Faults:    "); print_dec(sys_health.total_page_faults); println("");
    print("  Total Context Sws:    "); print_dec(sys_health.total_context_switches); println("");
    
    println("");
    println("Health Checks:");
    for (int i = 0; i < health_check_count; i++) {
        if (health_checks[i].name != NULL) {
            print("  ");
            print(health_checks[i].name);
            print(": ");
            println(status_names[health_checks[i].status]);
            print("    Failures: ");
            print_dec(health_checks[i].fail_count);
            print(" Recoveries: ");
            print_dec(health_checks[i].recover_count);
            println("");
        }
    }
}

/* 自动恢复尝试 */
int health_attempt_recovery(int type) {
    println("[HEALTH] Attempting recovery...");
    
    switch (type) {
        case 0:  /* CPU */
            sys_health.cpu_errors = 0;
            break;
        case 1:  /* Memory */
            /* 尝试内存整理 */
            sys_health.mem_errors = 0;
            break;
        case 2:  /* I/O */
            sys_health.io_errors = 0;
            break;
        case 3:  /* Network */
            sys_health.net_errors = 0;
            break;
        default:
            return -1;
    }
    
    println("[HEALTH] Recovery complete");
    return 0;
}

/* 系统看门狗 */
static uint64_t last_heartbeat = 0;
static int watchdog_enabled = 0;

/* 启用看门狗 */
void watchdog_enable(int enable) {
    watchdog_enabled = enable;
    if (enable) {
        last_heartbeat = get_tick_count();
    }
}

/* 看门狗心跳 */
void watchdog_heartbeat(void) {
    if (watchdog_enabled) {
        last_heartbeat = get_tick_count();
    }
}

/* 检查看门狗超时 */
int watchdog_check_timeout(uint64_t timeout_ns) {
    if (!watchdog_enabled) return 0;
    
    uint64_t now = get_tick_count();
    if (now - last_heartbeat > timeout_ns) {
        println("[WATCHDOG] Timeout detected!");
        return 1;
    }
    
    return 0;
}
