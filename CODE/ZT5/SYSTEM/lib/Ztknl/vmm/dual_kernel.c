/*
 * Zratro OS - Dual Kernel Manager
 *
 * 管理两个同时运行的内核（Ztknl 和 Azknl）
 * 支持双向对等架构：任一内核可作为主内核启动另一个
 */

#include "dual_kernel.h"
#include "vmx.h"
#include "ept.h"
#include "inter_kernel.h"
#include "console.h"
#include "string.h"
#include "pmm.h"
#include "elf.h"
#include "limine_file.h"
#include "vmm.h"
#include "heap.h"
#include "gdt.h"
#include "cpu.h"

#define SECONDARY_KERNEL_GPA_BASE    0x04200000  /* 副内核 GPA 基址 */
#define SECONDARY_KERNEL_MAX_SIZE     (256 * 1024 * 1024)  /* 最大 256MB */

/* ========== 辅助函数 ========== */

/* 读取 CR3 */
static inline uint64_t get_cr3(void) {
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

/* 读取 IDTR */
static inline uint64_t get_idtr_base(void) {
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr;
    asm volatile("sidt %0" : "=m"(idtr));
    return idtr.base;
}

/* ========== 全局状态 ========== */

static struct dual_kernel_manager dk_manager;
static int initialized = 0;

/* 副内核加载信息 */
static struct {
    uint8_t *image_data;      /* 内核镜像数据 */
    size_t image_size;         /* 内核大小 */
    uint64_t entry_point;      /* 入口点地址 */
    int loaded;                /* 是否已加载 */
} secondary_kernel = {0};

/* ========== 共享内存初始化 ========== */

static int init_shared_memory(void) {
    /* 分配共享内存页面 */
    dk_manager.shm = (struct inter_kernel_shm *)INTER_KERNEL_SHM_PHYS;
    
    if (!dk_manager.shm) {
        print("[DK] Failed to map shared memory\n");
        return -1;
    }
    
    /* 清零共享内存 */
    memset((void *)dk_manager.shm, 0, sizeof(struct inter_kernel_shm));
    
    /* 初始化头部 */
    dk_manager.shm->header.magic = INTER_KERNEL_MAGIC;
    dk_manager.shm->header.version = INTER_KERNEL_VERSION;
    dk_manager.shm->header.shm_phys = INTER_KERNEL_SHM_PHYS;
    dk_manager.shm->header.shm_size = INTER_KERNEL_SHM_SIZE;
    
    /* 初始化队列容量 */
    dk_manager.shm->ztknl_to_azknl.capacity = INTER_KERNEL_QUEUE_MSG_COUNT;
    dk_manager.shm->azknl_to_ztknl.capacity = INTER_KERNEL_QUEUE_MSG_COUNT;
    
    print("[DK] Shared memory initialized at 0x");
    char buf[17];
    uint64_t addr = INTER_KERNEL_SHM_PHYS;
    for (int i = 15; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[addr & 0xF];
        addr >>= 4;
    }
    buf[16] = '\0';
    println(buf);
    
    return 0;
}

/* ========== VMX 配置 ========== */

static int setup_vmx_for_guest(struct vm_state *vm, uint64_t entry) {
    /* 设置 VPID */
    vmcs_write(VIRTUAL_PROCESSOR_ID, vm->vpid);
    
    /* CPU-based Controls */
    uint32_t cpu_ctrl = 0;
    cpu_ctrl |= CPU_BASED_ACTIVATE_SECONDARY_CONTROLS;
    cpu_ctrl |= CPU_BASED_HLT_EXITING;
    cpu_ctrl |= CPU_BASED_INVLPG_EXITING;
    cpu_ctrl |= CPU_BASED_MWAIT_EXITING;
    cpu_ctrl |= CPU_BASED_RDTSC_EXITING;
    cpu_ctrl |= CPU_BASED_CR8_LOAD_EXITING;
    cpu_ctrl |= CPU_BASED_CR8_STORE_EXITING;
    cpu_ctrl |= CPU_BASED_UNCOND_IO_EXITING;
    cpu_ctrl |= CPU_BASED_MOV_DR_EXITING;
    cpu_ctrl |= CPU_BASED_PAUSE_EXITING;
    cpu_ctrl |= CPU_BASED_ACTIVATE_MSR_BITMAP;  /* 启用 MSR bitmap */
    vmcs_write(CPU_BASED_VM_EXEC_CONTROL, cpu_ctrl);
    
    /* Secondary Controls */
    uint32_t ctrl2 = 0;
    ctrl2 |= SECONDARY_EXEC_ENABLE_EPT;
    ctrl2 |= SECONDARY_EXEC_ENABLE_VPID;
    ctrl2 |= SECONDARY_EXEC_UNRESTRICTED_GUEST;
    vmcs_write(SECONDARY_VM_EXEC_CONTROL, ctrl2);
    
    /* VM-Exit Controls */
    uint32_t exit_ctrl = 0;
    exit_ctrl |= VM_EXIT_SAVE_DEBUG_CONTROLS;
    exit_ctrl |= VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL;
    exit_ctrl |= VM_EXIT_ACK_INTR_ON_EXIT;
    exit_ctrl |= VM_EXIT_SAVE_GUEST_EFER;
    exit_ctrl |= VM_EXIT_LOAD_HOST_EFER;
    vmcs_write(VM_EXIT_CONTROLS, exit_ctrl);
    
    /* VM-Entry Controls */
    uint32_t entry_ctrl_val = 0;
    entry_ctrl_val |= VM_ENTRY_LOAD_DEBUG_CONTROLS;
    entry_ctrl_val |= VM_ENTRY_IA32E_MODE_GUEST;
    entry_ctrl_val |= VM_ENTRY_LOAD_GUEST_EFER;
    vmcs_write(VM_ENTRY_CONTROLS, entry_ctrl_val);
    
    /* 设置 EPTP */
    uint64_t eptp = ept_construct_eptp();
    vmcs_write(EPT_POINTER, eptp);
    print("[DK] EPTP set to 0x");
    char buf2[20];
    uint64_t addr = eptp;
    for (int i = 15; i >= 0; i--) {
        buf2[i] = "0123456789ABCDEF"[addr & 0xF];
        addr >>= 4;
    }
    buf2[16] = '\0';
    println(buf2);

    /* Host 状态 */
    vmcs_write(HOST_CR0, 0x80000021);  /* PG|WP|NE|PE */
    vmcs_write(HOST_CR3, get_cr3());
    vmcs_write(HOST_CR4, 0x2000);      /* VMXE */
    vmcs_write(HOST_RSP, (uint64_t)&dk_manager.host_stack);
    vmcs_write(HOST_RIP, (uint64_t)vm_exit_handler);
    vmcs_write(HOST_FS_BASE, 0);
    vmcs_write(HOST_GS_BASE, 0);
    vmcs_write(HOST_GDTR_BASE, (uint64_t)gdt_get_ptr());
    vmcs_write(HOST_IDTR_BASE, get_idtr_base());  /* 修复：使用实际 IDT 基址 */
    
    /* Guest 状态 */
    vmcs_write(GUEST_CR0, 0x80000021);  /* PG|WP|NE|PE */
    /* CR3 将由客户机自行设置，这里设置为 GPA 0 */
    vmcs_write(GUEST_CR3, 0);
    vmcs_write(GUEST_CR4, 0x600);  /* PGE|MCE|PAE|PSE|DE */
    vmcs_write(GUEST_RSP, 0x8000);  /* 初始栈指针 */
    vmcs_write(GUEST_RIP, entry);
    vmcs_write(GUEST_RFLAGS, 0x2);  /* 固定位 */
    
    /* 段选择子 */
    vmcs_write(GUEST_CS_SELECTOR, 0x08);
    vmcs_write(GUEST_DS_SELECTOR, 0x10);
    vmcs_write(GUEST_ES_SELECTOR, 0x10);
    vmcs_write(GUEST_SS_SELECTOR, 0x10);
    vmcs_write(GUEST_FS_SELECTOR, 0);
    vmcs_write(GUEST_GS_SELECTOR, 0);
    vmcs_write(GUEST_TR_SELECTOR, 0);
    vmcs_write(GUEST_LDTR_SELECTOR, 0);
    
    /* 段基址 */
    vmcs_write(GUEST_CS_BASE, 0);
    vmcs_write(GUEST_DS_BASE, 0);
    vmcs_write(GUEST_ES_BASE, 0);
    vmcs_write(GUEST_SS_BASE, 0);
    vmcs_write(GUEST_FS_BASE, 0);
    vmcs_write(GUEST_GS_BASE, 0);
    vmcs_write(GUEST_TR_BASE, 0);
    vmcs_write(GUEST_GDTR_BASE, 0);
    vmcs_write(GUEST_IDTR_BASE, 0);
    
    /* 段限制和访问权限 */
    vmcs_write(GUEST_CS_LIMIT, 0xFFFFFFFF);
    vmcs_write(GUEST_DS_LIMIT, 0xFFFFFFFF);
    vmcs_write(GUEST_ES_LIMIT, 0xFFFFFFFF);
    vmcs_write(GUEST_SS_LIMIT, 0xFFFFFFFF);
    vmcs_write(GUEST_CS_AR_BYTES, 0xA09B);  /* Code, DPL0, Present */
    vmcs_write(GUEST_DS_AR_BYTES, 0xC093);  /* Data, DPL0, Present */
    vmcs_write(GUEST_ES_AR_BYTES, 0xC093);
    vmcs_write(GUEST_SS_AR_BYTES, 0xC093);
    vmcs_write(GUEST_FS_AR_BYTES, 0xC093);
    vmcs_write(GUEST_GS_AR_BYTES, 0xC093);
    
    /* 异常位图 - 捕获所有异常 */
    vmcs_write(EXCEPTION_BITMAP, 0xFFFFFFFF);
    
    return 0;
}

/* ========== 内存映射 ========== */

static int map_secondary_kernel_memory(void) {
    print("[DK] Mapping secondary kernel memory...\n");
    
    if (!secondary_kernel.loaded) {
        print("[DK] No secondary kernel loaded\n");
        return -1;
    }
    
    /* 计算需要的页面数 */
    size_t pages = (secondary_kernel.image_size + 4095) / 4096;
    print("[DK] Mapping ");
    char buf[20];
    int n = pages;
    int pos = 0;
    if (n == 0) buf[pos++] = '0';
    else {
        char tmp[20];
        int t = 0;
        while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
        while (t > 0) buf[pos++] = tmp[--t];
    }
    buf[pos] = '\0';
    print(buf);
    println(" pages");
    
    /* 映射副内核镜像到客户机物理地址 */
    /* 使用 1:1 映射：GPA 0x04200000 -> HPA of image */
    uint64_t image_hpa = (uint64_t)secondary_kernel.image_data;
    
    for (size_t i = 0; i < pages; i++) {
        uint64_t gpa = SECONDARY_KERNEL_GPA_BASE + i * 4096;
        uint64_t hpa = image_hpa + i * 4096;
        
        uint64_t flags = EPT_READ_ACCESS | EPT_WRITE_ACCESS | EPT_EXECUTE_ACCESS;
        if (ept_map_page(gpa, hpa, flags) != 0) {
            print("[DK] Failed to map page ");
            n = i;
            pos = 0;
            if (n == 0) buf[pos++] = '0';
            else {
                char tmp[20];
                int t = 0;
                while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
                while (t > 0) buf[pos++] = tmp[--t];
            }
            buf[pos] = '\0';
            println(buf);
            return -1;
        }
    }
    
    print("[DK] Secondary kernel mapped GPA 0x04200000 -> HPA 0x");
    uint64_t addr = image_hpa;
    for (int i = 15; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[addr & 0xF];
        addr >>= 4;
    }
    buf[16] = '\0';
    println(buf);
    
    return 0;
}

static int map_shared_memory_to_ept(void) {
    print("[DK] Mapping shared memory...\n");
    
    /* 映射共享内存到 EPT */
    uint64_t shm_gpa = INTER_KERNEL_SHM_PHYS;
    uint64_t shm_hpa = INTER_KERNEL_SHM_PHYS;  /* 1:1 映射 */
    
    size_t pages = INTER_KERNEL_SHM_SIZE / 4096;
    
    for (size_t i = 0; i < pages; i++) {
        uint64_t gpa = shm_gpa + i * 4096;
        uint64_t hpa = shm_hpa + i * 4096;
        
        uint64_t flags = EPT_READ_ACCESS | EPT_WRITE_ACCESS;
        if (ept_map_page(gpa, hpa, flags) != 0) {
            print("[DK] Failed to map shared memory page\n");
            return -1;
        }
    }
    
    print("[DK] Shared memory mapped\n");
    return 0;
}

/* ========== VM-Exit 处理 ========== */

static void handle_exception_nmi(void) {
    uint32_t intr_info = vmcs_read(0x4404);
    uint32_t intr_error = vmcs_read(0x4406) & 0xFFFFFFFF;
    uint64_t rip = vmcs_read(0x681E);
    uint64_t rsp = vmcs_read(0x681C);
    uint64_t cr2 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    
    print("[VM-Exit] Exception/NMI\n");
    
    (void)intr_info;
    (void)intr_error;
    (void)rip;
    (void)rsp;
    (void)cr2;
}

static void handle_ept_violation(void) {
    uint64_t gpa = vmcs_read(0x2400);
    uint64_t gla = vmcs_read(0x6400);
    uint32_t exit_qual = vmcs_read(0x6402) & 0xFFFFFFFF;
    uint64_t rip = vmcs_read(0x681E);
    
    print("[VM-Exit] EPT Violation\n");
    
    /* 尝试 1:1 映射 */
    uint64_t page_gpa = gpa & ~0xFFFULL;
    uint64_t flags = EPT_READ_ACCESS | EPT_WRITE_ACCESS | EPT_EXECUTE_ACCESS;
    ept_map_page(page_gpa, page_gpa, flags);
    
    (void)gla;
    (void)exit_qual;
    (void)rip;
}

static void handle_io_instruction(void) {
    uint64_t rip = vmcs_read(0x681E);
    uint64_t qualification = vmcs_read(0x6404);
    uint64_t port = (qualification >> 16) & 0xFFFF;
    
    print("[VM-Exit] I/O instruction\n");
    
    /* 跳过 I/O 指令 */
    uint64_t instr_len = vmcs_read(0x440C) & 0xFFFFFFFF;
    rip += instr_len;
    vmcs_write(0x681E, rip);
    
    (void)port;
}

static void vmx_exit_handler(void) {
    uint32_t exit_reason = vmcs_read(0x4402) & 0xFFFF;
    dk_manager.vm_exits++;
    
    switch (exit_reason) {
        case 1:  /* EXCEPTION_NMI */
            handle_exception_nmi();
            break;
        case 3:  /* INIT */
            print("[VM-Exit] INIT signal\n");
            break;
        case 6:  /* HLT */
            print("[VM-Exit] HLT instruction\n");
            {
                uint64_t rip = vmcs_read(0x681E);
                uint64_t instr_len = vmcs_read(0x440C) & 0xFFFFFFFF;
                vmcs_write(0x681E, rip + instr_len);
            }
            break;
        case 10: /* INVD */
            print("[VM-Exit] INVD instruction\n");
            {
                uint64_t rip = vmcs_read(0x681E);
                uint64_t instr_len = vmcs_read(0x440C) & 0xFFFFFFFF;
                vmcs_write(0x681E, rip + instr_len);
            }
            break;
        case 30: /* MWAIT */
            print("[VM-Exit] MWAIT instruction\n");
            {
                uint64_t rip = vmcs_read(0x681E);
                uint64_t instr_len = vmcs_read(0x440C) & 0xFFFFFFFF;
                vmcs_write(0x681E, rip + instr_len);
            }
            break;
        case 40: /* RDMSR */
            print("[VM-Exit] RDMSR instruction\n");
            {
                uint64_t rip = vmcs_read(0x681E);
                uint64_t instr_len = vmcs_read(0x440C) & 0xFFFFFFFF;
                vmcs_write(0x681E, rip + instr_len);
            }
            break;
        case 41: /* WRMSR */
            print("[VM-Exit] WRMSR instruction\n");
            {
                uint64_t rip = vmcs_read(0x681E);
                uint64_t instr_len = vmcs_read(0x440C) & 0xFFFFFFFF;
                vmcs_write(0x681E, rip + instr_len);
            }
            break;
        case 48: /* VMCALL */
            print("[VM-Exit] VMCALL instruction\n");
            {
                uint64_t rip = vmcs_read(0x681E);
                uint64_t instr_len = vmcs_read(0x440C) & 0xFFFFFFFF;
                vmcs_write(0x681E, rip + instr_len);
            }
            break;
        case 49: /* EPT Violation */
            handle_ept_violation();
            break;
        case 50: /* I/O Instruction */
            handle_io_instruction();
            break;
        default:
            print("[VM-Exit] Unknown reason: 0x");
            char buf1[8];
            uint32_t n = exit_reason;
            for (int i = 3; i >= 0; i--) {
                buf1[i] = "0123456789ABCDEF"[n & 0xF];
                n >>= 4;
            }
            buf1[4] = '\0';
            print(buf1);
            print(" at RIP=0x");
            char buf2[20];
            uint64_t addr = vmcs_read(0x681E);
            for (int i = 15; i >= 0; i--) {
                buf2[i] = "0123456789ABCDEF"[addr & 0xF];
                addr >>= 4;
            }
            buf2[16] = '\0';
            println(buf2);
            break;
    }
    
    /* VM-Resume */
    asm volatile("vmresume" : : : "memory");
}

void vm_exit_handler(void) {
    vmx_exit_handler();
}

/* ========== 公共 API ========== */

int dual_kernel_init(int is_primary, const char *secondary_image) {
    if (initialized) {
        print("[DK] Already initialized\n");
        return 0;
    }
    
    print("[DK] Initializing dual kernel system...\n");
    
    /* 初始化管理器 */
    memset(&dk_manager, 0, sizeof(dk_manager));
    dk_manager.is_primary = is_primary;
    dk_manager.state = DK_STATE_INIT;
    
    /* 启用 VMX */
    if (vmx_enable() != 0) {
        print("[DK] Failed to enable VMX\n");
        /* 通知vmm.c双内核初始化失败 */
        extern void vmm_set_dual_kernel_status(int status);
        vmm_set_dual_kernel_status(0);
        return -1;
    }
    
    /* 初始化 EPT */
    if (ept_init() != 0) {
        print("[DK] Failed to initialize EPT\n");
        return -1;
    }
    
    /* 初始化共享内存 */
    if (init_shared_memory() != 0) {
        print("[DK] Failed to initialize shared memory\n");
        return -1;
    }
    
    /* 如果是主内核，加载副内核 */
    if (is_primary && secondary_image) {
        print("[DK] Loading secondary kernel: ");
        println(secondary_image);
        
        if (dual_kernel_load(secondary_image) != 0) {
            print("[DK] Failed to load secondary kernel\n");
            return -1;
        }
    }
    
    /* 创建虚拟机 */
    if (vm_create(&dk_manager.secondary_vm) != 0) {
        print("[DK] Failed to create VM\n");
        return -1;
    }
    
    /* 初始化核间通信 */
    inter_kernel_init();
    
    dk_manager.state = DK_STATE_RUNNING;
    initialized = 1;
    
    /* 通知vmm.c双内核初始化成功 */
    extern void vmm_set_dual_kernel_status(int status);
    vmm_set_dual_kernel_status(1);
    
    print("[DK] Dual kernel system ready\n");
    return 0;
}

int dual_kernel_load(const char *path) {
    if (!initialized) {
        print("[DK] Dual kernel not initialized\n");
        return -1;
    }
    
    if (secondary_kernel.loaded) {
        print("[DK] Secondary kernel already loaded\n");
        return 0;
    }
    
    print("[DK] Loading kernel from: ");
    println(path);
    
    /* 尝试从 Limine 模块加载 */
    void *data = limine_find_module(path, NULL);
    if (!data) {
        print("[DK] Failed to load from Limine modules\n");
        return -1;
    }
    
    /* 验证 ELF 文件 */
    struct elf64_hdr *hdr = (struct elf64_hdr *)data;
    if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L' || hdr->e_ident[3] != 'F') {
        print("[DK] Invalid ELF magic\n");
        return -1;
    }
    
    if (hdr->e_type != ELF_TYPE_EXEC) {
        print("[DK] Not an executable ELF\n");
        return -1;
    }
    
    if (hdr->e_machine != ELF_MACHINE_X86_64) {
        print("[DK] Not x86_64 ELF\n");
        return -1;
    }
    
    /* 获取内核大小 */
    extern void *limine_get_kernel_file(size_t *size_out);
    size_t size;
    (void)limine_get_kernel_file(&size);
    
    /* 保存内核信息 */
    secondary_kernel.image_data = (uint8_t *)data;
    secondary_kernel.image_size = size;
    secondary_kernel.entry_point = hdr->e_entry;
    secondary_kernel.loaded = 1;
    
    print("[DK] Entry point: 0x");
    char buf[20];
    uint64_t addr = secondary_kernel.entry_point;
    for (int i = 15; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[addr & 0xF];
        addr >>= 4;
    }
    buf[16] = '\0';
    println(buf);
    
    print("[DK] Kernel loaded successfully\n");
    return 0;
}

int dual_kernel_start_secondary(void) {
    if (!initialized || !dk_manager.secondary_vm.vmcs) {
        print("[DK] VM not initialized\n");
        return -1;
    }
    
    if (!secondary_kernel.loaded) {
        print("[DK] Secondary kernel not loaded\n");
        return -1;
    }
    
    print("[DK] Starting secondary kernel...\n");
    
    /* 映射副内核内存 */
    if (map_secondary_kernel_memory() != 0) {
        print("[DK] Failed to map secondary kernel memory\n");
        return -1;
    }
    
    /* 映射共享内存 */
    if (map_shared_memory_to_ept() != 0) {
        print("[DK] Failed to map shared memory\n");
        return -1;
    }
    
    /* 配置 VMX */
    /* 计算客户机入口点地址：GPA_BASE + (entry - load_addr) */
    uint64_t guest_entry = SECONDARY_KERNEL_GPA_BASE +
                          (secondary_kernel.entry_point - (uint64_t)secondary_kernel.image_data);
    
    print("[DK] Guest entry point: 0x");
    char buf[20];
    uint64_t addr = guest_entry;
    for (int i = 15; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[addr & 0xF];
        addr >>= 4;
    }
    buf[16] = '\0';
    println(buf);
    
    /* 设置 VMCS */
    if (setup_vmx_for_guest(&dk_manager.secondary_vm, guest_entry) != 0) {
        print("[DK] Failed to setup VMX\n");
        return -1;
    }
    
    /* 启动虚拟机 */
    dk_manager.secondary_vm.launched = 0;  /* 首次启动 */
    vm_run(&dk_manager.secondary_vm);
    
    return 0;
}

int dual_kernel_send_msg(uint64_t type, const uint64_t *data, size_t count) {
    return inter_kernel_send_msg(type, data, count);
}

int dual_kernel_recv_msg(struct inter_kernel_msg *msg) {
    return inter_kernel_recv_msg(msg);
}

struct dual_kernel_manager *dual_kernel_get_manager(void) {
    return &dk_manager;
}
