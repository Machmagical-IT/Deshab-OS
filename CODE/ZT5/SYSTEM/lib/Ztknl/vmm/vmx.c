/*
 * Zratro OS - VMX Implementation
 */

#include "vmx.h"
#include "console.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"

/* VMXON 区域 */
static uint64_t *vmxon_region = NULL;
static uint64_t vmxon_phys = 0;
static int vmx_enabled = 0;

/* 基本 VMCS 修订 ID */
static uint32_t vmcs_revision_id = 0;

/* ========== VMX 指令包装 ========== */

static inline uint8_t vmcall(void) {
    uint8_t error;
    asm volatile("vmcall" : "=a"(error) : : "memory");
    return error;
}

static inline uint8_t vmxon(uint64_t phys) {
    uint8_t error;
    asm volatile("vmxon %1" : "=a"(error) : "m"(phys) : "memory");
    return error;
}

static inline uint8_t vmxoff(void) {
    uint8_t error;
    asm volatile("vmxoff" : "=a"(error) : : "memory");
    return error;
}

static inline uint8_t vmptrld(uint64_t phys) {
    uint8_t error;
    asm volatile("vmptrld %1" : "=a"(error) : "m"(phys) : "memory");
    return error;
}

static inline uint8_t vmclear(uint64_t phys) {
    uint8_t error;
    asm volatile("vmclear %1" : "=a"(error) : "m"(phys) : "memory");
    return error;
}

static inline uint64_t vmread_vmcs(uint32_t field) {
    uint64_t value;
    asm volatile("vmread %0, %1" 
                 : "=r"(value) 
                 : "r"((uint64_t)field));
    return value;
}

static inline void vmwrite_vmcs(uint32_t field, uint64_t value) {
    asm volatile("vmwrite %0, %1" 
                 : 
                 : "r"(value), "r"((uint64_t)field));
}

uint64_t vmcs_read(uint32_t field) {
    if (!vmx_enabled) {
        return 0;
    }
    return vmread_vmcs(field);
}

int vmcs_write(uint32_t field, uint64_t value) {
    if (!vmx_enabled) {
        return -1;
    }
    vmwrite_vmcs(field, value);
    return 0;
}

/* ========== VMX 初始化 ========== */

int vmx_check_support(void) {
    uint32_t eax, ebx, ecx, edx;
    
    /* 检查 CPUID.1:ECX.VMX[bit 5] */
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (!(ecx & (1 << 5))) {
        print("[虚拟化] 处理器不支持VMX技术\n");
        return 0;
    }
    
    /* 读取 VMX 能力 MSR */
    uint64_t ia32_feature_control;
    asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(0x3A));
    ia32_feature_control = ((uint64_t)edx << 32) | eax;
    
    /* 检查 VMX 是否已启用 */
    if (!(ia32_feature_control & 0x4)) {
        print("[虚拟化] IA32_FEATURE_CONTROL中VMX已禁用\n");
        return 0;
    }
    
    /* 读取 VMCS 修订 ID */
    uint64_t vmx_basic;
    asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(0x480));
    vmx_basic = ((uint64_t)edx << 32) | eax;
    vmcs_revision_id = vmx_basic & 0x7FFFFFFF;
    
    print("[虚拟化] VMX支持已确认, 修订ID: 0x");
    char buf[9];
    uint32_t rev = vmcs_revision_id;
    for (int i = 7; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[rev & 0xF];
        rev >>= 4;
    }
    buf[8] = '\0';
    println(buf);
    
    return 1;
}

int vmx_enable(void) {
    if (vmx_enabled) {
        return 0;
    }
    
    if (!vmx_check_support()) {
        return -1;
    }
    
    /* 分配 VMXON 区域 */
    vmxon_region = (uint64_t *)pmm_alloc_page();
    if (!vmxon_region) {
        print("[虚拟化] 分配VMXON区域失败\n");
        return -1;
    }
    
    memset(vmxon_region, 0, 4096);
    vmxon_region[0] = vmcs_revision_id;
    vmxon_phys = (uint64_t)vmxon_region;
    
    /* 设置 CR4 */
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 13);  /* VMXE */
    asm volatile("mov %0, %%cr4" : : "r"(cr4));
    
    /* 执行 VMXON */
    if (vmxon(vmxon_phys) != 0) {
        print("[虚拟化] VMXON指令执行失败\n");
        return -1;
    }
    
    vmx_enabled = 1;
    print("[虚拟化] 虚拟化已成功启用\n");
    
    return 0;
}

void vmx_disable(void) {
    if (!vmx_enabled) {
        return;
    }
    
    vmxoff();
    vmx_enabled = 0;
    print("[虚拟化] 虚拟化已禁用\n");
}

/* ========== 虚拟机管理 ========== */

int vm_create(struct vm_state *state) {
    if (!state || !vmx_enabled) {
        return -1;
    }
    
    /* 分配 VMCS 区域 */
    state->vmcs = (struct vmcs *)pmm_alloc_page();
    if (!state->vmcs) {
        print("[虚拟机] 分配VMCS失败\n");
        return -1;
    }
    
    memset(state->vmcs, 0, VMCS_SIZE);
    state->vmcs->revision_id = vmcs_revision_id;
    state->vmcs_phys = (uint64_t)state->vmcs;
    state->launched = 0;
    state->vpid = 1;
    
    /* 清除 VMCS */
    if (vmclear(state->vmcs_phys) != 0) {
        print("[虚拟机] VMCLEAR失败\n");
        return -1;
    }
    
    /* 加载 VMCS 指针 */
    if (vmptrld(state->vmcs_phys) != 0) {
        print("[虚拟机] VMPTRLD失败\n");
        return -1;
    }
    
    print("[虚拟机] 虚拟机创建成功\n");
    return 0;
}

void vm_run(struct vm_state *state) {
    if (!state || !state->launched) {
        print("[虚拟机] 虚拟机未启动\n");
        return;
    }
    
    print("[虚拟机] 正在进入客户机...\n");
    
    /* VMLAUNCH 或 VMRESUME */
    if (!state->launched) {
        asm volatile("vmlaunch" : : : "memory");
    } else {
        asm volatile("vmresume" : : : "memory");
    }
    
    /* 如果返回到这里，说明发生了 VM-Exit */
    uint32_t exit_reason = (uint32_t)vmcs_read(0x4402);
    print("[虚拟机] VM-Exit退出! 原因: 0x");
    char buf[9];
    for (int i = 7; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[exit_reason & 0xF];
        exit_reason >>= 4;
    }
    buf[8] = '\0';
    println(buf);
}

void vm_stop(struct vm_state *state) {
    if (!state) {
        return;
    }
    
    if (state->vmcs) {
        vmclear(state->vmcs_phys);
        state->vmcs = NULL;
    }
    
    print("[虚拟机] 虚拟机已停止\n");
}
