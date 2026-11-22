/*
 * Zratro OS - VMX Hardware Virtualization
 * 
 * Intel VT-x 硬件虚拟化支持
 * 允许一个内核作为 VMM 运行另一个内核
 */

#ifndef VMX_H
#define VMX_H

#include <stdint.h>

/* ========== VMX 基础定义 ========== */

/* VMX 指令错误码 */
#define VMX_ERROR_SUCCESS           0
#define VMX_ERROR_VMCALL            1
#define VMX_ERROR_VMCLEAR           2
#define VMX_ERROR_VMLAUNCH          3
#define VMX_ERROR_VMPTRLD           4
#define VMX_ERROR_VMPTRST           5
#define VMX_ERROR_VMREAD            6
#define VMX_ERROR_VMRESUME          7
#define VMX_ERROR_VMWRITE           8
#define VMX_ERROR_VMXOFF            9
#define VMX_ERROR_VMXON             10

/* VMCS 字段编码 */
enum vmcs_field {
    /* 16-bit Control Fields */
    VIRTUAL_PROCESSOR_ID            = 0x00000000,
    EPTP_INDEX                      = 0x00000004,
    
    /* 16-bit Guest-State Fields */
    GUEST_ES_SELECTOR               = 0x00000800,
    GUEST_CS_SELECTOR               = 0x00000802,
    GUEST_SS_SELECTOR               = 0x00000804,
    GUEST_DS_SELECTOR               = 0x00000806,
    GUEST_FS_SELECTOR               = 0x00000808,
    GUEST_GS_SELECTOR               = 0x0000080A,
    GUEST_LDTR_SELECTOR             = 0x0000080C,
    GUEST_TR_SELECTOR               = 0x0000080E,
    
    /* 16-bit Host-State Fields */
    HOST_ES_SELECTOR                = 0x00000C00,
    HOST_CS_SELECTOR                = 0x00000C02,
    HOST_SS_SELECTOR                = 0x00000C04,
    HOST_DS_SELECTOR                = 0x00000C06,
    HOST_FS_SELECTOR                = 0x00000C08,
    HOST_GS_SELECTOR                = 0x00000C0A,
    HOST_TR_SELECTOR                = 0x00000C0C,
    
    /* 64-bit Control Fields */
    IO_BITMAP_A                     = 0x00002000,
    IO_BITMAP_B                     = 0x00002002,
    MSR_BITMAP                      = 0x00002004,
    VM_EXIT_MSR_STORE               = 0x00002006,
    VM_EXIT_MSR_LOAD                = 0x00002008,
    VM_ENTRY_MSR_LOAD               = 0x0000200A,
    TSC_OFFSET                      = 0x00002010,
    VIRTUAL_APIC_PAGE               = 0x00002012,
    APIC_ACCESS_PAGE                = 0x00002014,
    EPT_POINTER                     = 0x0000201A,
    
    /* 64-bit Host-State Fields */
    HOST_IA32_PAT_FULL              = 0x00002C00,
    HOST_IA32_EFER_FULL             = 0x00002C02,
    
    /* 32-bit Control Fields */
    PIN_BASED_VM_EXEC_CONTROL       = 0x00004000,
    CPU_BASED_VM_EXEC_CONTROL       = 0x00004002,
    EXCEPTION_BITMAP                = 0x00004004,
    PAGE_FAULT_ERROR_CODE_MASK      = 0x00004006,
    PAGE_FAULT_ERROR_CODE_MATCH     = 0x00004008,
    CR3_TARGET_COUNT                = 0x0000400A,
    VM_EXIT_CONTROLS                = 0x0000400C,
    VM_ENTRY_CONTROLS               = 0x00004012,
    VM_ENTRY_INTR_INFO              = 0x00004016,
    VM_ENTRY_INSTRUCTION_LEN        = 0x0000401A,
    SECONDARY_VM_EXEC_CONTROL       = 0x0000401E,
    
    /* 32-bit Guest-State Fields */
    GUEST_ES_LIMIT                  = 0x00004800,
    GUEST_CS_LIMIT                  = 0x00004802,
    GUEST_SS_LIMIT                  = 0x00004804,
    GUEST_DS_LIMIT                  = 0x00004806,
    GUEST_FS_LIMIT                  = 0x00004808,
    GUEST_GS_LIMIT                  = 0x0000480A,
    GUEST_LDTR_LIMIT                = 0x0000480C,
    GUEST_TR_LIMIT                  = 0x0000480E,
    GUEST_GDTR_LIMIT                = 0x00004810,
    GUEST_IDTR_LIMIT                = 0x00004812,
    GUEST_ES_AR_BYTES               = 0x00004814,
    GUEST_CS_AR_BYTES               = 0x00004816,
    GUEST_SS_AR_BYTES               = 0x00004818,
    GUEST_DS_AR_BYTES               = 0x0000481A,
    GUEST_FS_AR_BYTES               = 0x0000481C,
    GUEST_GS_AR_BYTES               = 0x0000481E,
    GUEST_LDTR_AR_BYTES             = 0x00004820,
    GUEST_TR_AR_BYTES               = 0x00004822,
    GUEST_INTERRUPTIBILITY_INFO     = 0x00004824,
    GUEST_ACTIVITY_STATE            = 0x00004826,
    GUEST_SYSENTER_CS               = 0x0000482A,
    
    /* 32-bit Host-State Fields */
    HOST_IA32_SYSENTER_CS           = 0x00004C00,
    
    /* Natural-Width Control Fields */
    CR0_GUEST_HOST_MASK             = 0x00006000,
    CR4_GUEST_HOST_MASK             = 0x00006002,
    CR0_READ_SHADOW                 = 0x00006004,
    CR4_READ_SHADOW                 = 0x00006006,
    
    /* Natural-Width Guest-State Fields */
    GUEST_CR0                       = 0x00006800,
    GUEST_CR3                       = 0x00006802,
    GUEST_CR4                       = 0x00006804,
    GUEST_ES_BASE                   = 0x00006806,
    GUEST_CS_BASE                   = 0x00006808,
    GUEST_SS_BASE                   = 0x0000680A,
    GUEST_DS_BASE                   = 0x0000680C,
    GUEST_FS_BASE                   = 0x0000680E,
    GUEST_GS_BASE                   = 0x00006810,
    GUEST_LDTR_BASE                 = 0x00006812,
    GUEST_TR_BASE                   = 0x00006814,
    GUEST_GDTR_BASE                 = 0x00006816,
    GUEST_IDTR_BASE                 = 0x00006818,
    GUEST_DR7                       = 0x0000681A,
    GUEST_RSP                       = 0x0000681C,
    GUEST_RIP                       = 0x0000681E,
    GUEST_RFLAGS                    = 0x00006820,
    GUEST_SYSENTER_ESP              = 0x00006824,
    GUEST_SYSENTER_EIP              = 0x00006826,
    
    /* Natural-Width Host-State Fields */
    HOST_CR0                        = 0x00006C00,
    HOST_CR3                        = 0x00006C02,
    HOST_CR4                        = 0x00006C04,
    HOST_FS_BASE                    = 0x00006C06,
    HOST_GS_BASE                    = 0x00006C08,
    HOST_TR_BASE                    = 0x00006C0A,
    HOST_GDTR_BASE                  = 0x00006C0C,
    HOST_IDTR_BASE                  = 0x00006C0E,
    HOST_RSP                        = 0x00006C14,
    HOST_RIP                        = 0x00006C16,
};

/* CPU-based VM-Execution Controls */
#define CPU_BASED_VIRTUAL_INTR_PENDING      (1u << 2)
#define CPU_BASED_USE_TSC_OFFSETING         (1u << 3)
#define CPU_BASED_HLT_EXITING               (1u << 7)
#define CPU_BASED_INVLPG_EXITING            (1u << 9)
#define CPU_BASED_MWAIT_EXITING             (1u << 10)
#define CPU_BASED_RDPMC_EXITING             (1u << 11)
#define CPU_BASED_RDTSC_EXITING             (1u << 12)
#define CPU_BASED_CR8_LOAD_EXITING          (1u << 19)
#define CPU_BASED_CR8_STORE_EXITING         (1u << 20)
#define CPU_BASED_TPR_SHADOW                (1u << 21)
#define CPU_BASED_MOV_DR_EXITING            (1u << 23)
#define CPU_BASED_UNCOND_IO_EXITING         (1u << 24)
#define CPU_BASED_ACTIVATE_IO_BITMAP        (1u << 25)
#define CPU_BASED_ACTIVATE_MSR_BITMAP       (1u << 28)
#define CPU_BASED_MONITOR_EXITING           (1u << 29)
#define CPU_BASED_PAUSE_EXITING             (1u << 30)
#define CPU_BASED_ACTIVATE_SECONDARY_CONTROLS (1u << 31)

/* Secondary VM-Execution Controls */
#define SECONDARY_EXEC_ENABLE_EPT               (1u << 1)
#define SECONDARY_EXEC_ENABLE_RDTSCP            (1u << 3)
#define SECONDARY_EXEC_ENABLE_VPID              (1u << 5)
#define SECONDARY_EXEC_UNRESTRICTED_GUEST       (1u << 7)
#define SECONDARY_EXEC_ENABLE_INVPCID           (1u << 12)
#define SECONDARY_EXEC_ENABLE_VMCS_SHADOWING    (1u << 14)
#define SECONDARY_EXEC_XSAVES                   (1u << 19)

/* VM-Exit Controls */
#define VM_EXIT_SAVE_DEBUG_CONTROLS             (1u << 2)
#define VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL      (1u << 12)
#define VM_EXIT_ACK_INTR_ON_EXIT                (1u << 15)
#define VM_EXIT_SAVE_GUEST_EFER                 (1u << 20)
#define VM_EXIT_LOAD_HOST_EFER                  (1u << 21)

/* VM-Entry Controls */
#define VM_ENTRY_LOAD_DEBUG_CONTROLS            (1u << 2)
#define VM_ENTRY_IA32E_MODE_GUEST               (1u << 9)
#define VM_ENTRY_LOAD_GUEST_EFER                (1u << 15)

/* EPT 相关 */
#define EPT_READ_ACCESS                         (1u << 0)
#define EPT_WRITE_ACCESS                        (1u << 1)
#define EPT_EXECUTE_ACCESS                      (1u << 2)
#define EPT_MEMORY_TYPE_WB                      6

/* VMCS 大小 */
#define VMCS_SIZE                               4096

/* ========== 数据结构 ========== */

/* 段访问权限字节 */
struct segment_ar {
    uint32_t type : 4;
    uint32_t s : 1;
    uint32_t dpl : 2;
    uint32_t p : 1;
    uint32_t reserved : 4;
    uint32_t avl : 1;
    uint32_t l : 1;
    uint32_t db : 1;
    uint32_t g : 1;
    uint32_t unusable : 1;
} __attribute__((packed));

/* VMCS 区域 */
struct vmcs {
    uint32_t revision_id;
    uint32_t abort_indicator;
    uint8_t data[VMCS_SIZE - 8];
} __attribute__((aligned(4096)));

/* 虚拟机状态 */
struct vm_state {
    struct vmcs *vmcs;
    uint64_t vmcs_phys;
    int launched;
    uint16_t vpid;
} __attribute__((aligned(64)));

/* ========== API 接口 ========== */

/**
 * 检查 VMX 支持
 * @return 1 支持，0 不支持
 */
int vmx_check_support(void);

/**
 * 启用 VMX
 * @return 0 成功，-1 失败
 */
int vmx_enable(void);

/**
 * 禁用 VMX
 */
void vmx_disable(void);

/**
 * 创建虚拟机
 * @param state 虚拟机状态
 * @return 0 成功，-1 失败
 */
int vm_create(struct vm_state *state);

/**
 * 启动虚拟机
 * @param state 虚拟机状态
 */
void vm_run(struct vm_state *state);

/**
 * 停止虚拟机
 * @param state 虚拟机状态
 */
void vm_stop(struct vm_state *state);

/**
 * 设置 VMCS 字段
 */
int vmcs_write(uint32_t field, uint64_t value);

/**
 * 读取 VMCS 字段
 */
uint64_t vmcs_read(uint32_t field);

#endif /* VMX_H */
