/* zt5_boot.c — VMCS 配置与 vmlaunch for ZT5 guest
 *
 * ZT5 `_start` 无参数；RIP=0xFFFFFFFF80000000+ 入口，RSP=guest boot stack。
 * 与 xj380_boot.c 的差异：
 *   - VPID=4
 *   - MSR bitmap 拦截 LAPIC MSR 区（0x800-0x8FF）与 IA32_APIC_BASE(0x1B)：
 *     ZT5 的 APIC 初始化在 guest 内不可用（guest profile 改走 PIT），拦截后
 *     读写按 0 处理，避免污染 host LAPIC。
 */
#include <utsm/zt5_loader.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/vmm.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;
extern void vmx_vm_exit_handler(void);
extern int vmexit_dispatch(void);
extern u64 vmexit_get_count(void);

static inline u64 read_cr0_local(void) {
    u64 v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline u64 read_cr3_local(void) {
    u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline u64 read_cr4_local(void) {
    u64 v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v;
}

static u64 zt5_adjust_control(u64 value, u32 msr) {
    u32 allowed0 = (u32)vmx_read_msr(msr);
    u32 allowed1 = (u32)(vmx_read_msr(msr) >> 32);
    u32 adjusted = (u32)value | allowed0;
    adjusted &= allowed1;
    return adjusted;
}

static u8 g_zt5_msr_bitmap[4096] __attribute__((aligned(4096)));

static void msr_bitmap_intercept(u32 msr) {
    if (msr < 0x2000) {
        g_zt5_msr_bitmap[msr / 8] |= (u8)(1u << (msr % 8));             /* read */
        g_zt5_msr_bitmap[1024 + msr / 8] |= (u8)(1u << (msr % 8));      /* write */
    } else {
        u32 off = msr - 0xC0000000u;
        g_zt5_msr_bitmap[2048 + off / 8] |= (u8)(1u << (off % 8));
        g_zt5_msr_bitmap[3072 + off / 8] |= (u8)(1u << (off % 8));
    }
}

/* ===== host state ===== */
static void zt5_vmcs_setup_host_state(void) {
    vmx_vmcs_write(VMCS_HOST_CR0, read_cr0_local() | CR0_PE | CR0_NE | CR0_PG);
    vmx_vmcs_write(VMCS_HOST_CR3, read_cr3_local());
    vmx_vmcs_write(VMCS_HOST_CR4, read_cr4_local() | CR4_VMXE);
    vmx_vmcs_write(VMCS_HOST_CS_SELECTOR, 0x08);
    vmx_vmcs_write(VMCS_HOST_SS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_DS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_ES_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_FS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_HOST_GS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_HOST_FS_BASE, 0);
    vmx_vmcs_write(VMCS_HOST_GS_BASE, vmx_read_msr(0xC0000101));
    {
        struct __attribute__((packed)) { u16 limit; u64 base; } gdtr, idtr;
        __asm__ volatile("sgdt %0" : "=m"(gdtr));
        __asm__ volatile("sidt %0" : "=m"(idtr));
        vmx_vmcs_write(VMCS_HOST_GDTR_BASE, gdtr.base);
        vmx_vmcs_write(VMCS_HOST_IDTR_BASE, idtr.base);
        u16 tr_sel;
        __asm__ volatile("str %0" : "=r"(tr_sel));
        u64 *tss_desc = (u64 *)(gdtr.base + (tr_sel & 0xFFF8));
        u64 low = tss_desc[0], high = tss_desc[1];
        u64 tr_base = (low >> 16) & 0xFFFFFF;
        tr_base |= ((low >> 32) & 0xFF) << 24;
        tr_base |= (high & 0xFFFFFFFF) << 32;
        vmx_vmcs_write(VMCS_HOST_TR_SELECTOR, tr_sel);
        vmx_vmcs_write(VMCS_HOST_TR_BASE, tr_base);
    }
    {
        u64 sysenter_cs;
        __asm__ volatile("rdmsr" : "=a"(sysenter_cs) : "c"(0x174));
        vmx_vmcs_write(VMCS_HOST_SYSENTER_CS, sysenter_cs & 0xFFFFFFFF);
        vmx_vmcs_write(VMCS_HOST_SYSENTER_ESP, 0);
        vmx_vmcs_write(VMCS_HOST_SYSENTER_EIP, 0);
    }
    {
        u32 low, high;
        __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
        vmx_vmcs_write(VMCS_HOST_IA32_EFER, ((u64)high << 32) | low);
    }
    vmx_vmcs_write(VMCS_HOST_RSP, vmm_get_host_stack_top());
    vmx_vmcs_write(VMCS_HOST_RIP, (u64)vmx_vm_exit_handler);
}

/* ===== guest state ===== */
static void zt5_vmcs_setup_guest_state(const struct zt5_guest_info *gi) {
    /* CR4 必须含 VMXE（IA32_VMX_CR4_FIXED0 强制）；CR0 需 MP/ET。
     * 缺少会触发 VM-entry invalid guest state（error 7）。
     * OSFXSR/OSXMMEXCPT 供 guest 用户态 SSE 指令使用。 */
    u64 cr0 = CR0_PE | CR0_MP | CR0_ET | CR0_NE | CR0_PG | CR0_WP;
    u64 cr4 = CR4_VMXE | CR4_PAE | CR4_PGE | CR4_PSE |
              CR4_OSFXSR | CR4_OSXMMEXCPT;
    vmx_vmcs_write(VMCS_GUEST_CR0, cr0);
    vmx_vmcs_write(VMCS_GUEST_CR3, gi->pgt_gpa);
    vmx_vmcs_write(VMCS_GUEST_CR4, cr4);
    {
        u64 cr0_fixed0 = vmx_read_msr(IA32_VMX_CR0_FIXED0) & 0xFFFFFFFFULL;
        u64 cr0_fixed1 = vmx_read_msr(IA32_VMX_CR0_FIXED1) & 0xFFFFFFFFULL;
        u64 cr4_fixed0 = vmx_read_msr(IA32_VMX_CR4_FIXED0) & 0xFFFFFFFFULL;
        u64 cr0_mask = cr0_fixed0 | (~cr0_fixed1 & 0xFFFFFFFFULL);
        vmx_vmcs_write(VMCS_CR0_GUEST_HOST_MASK, cr0_mask);
        vmx_vmcs_write(VMCS_CR0_READ_SHADOW, cr0);
        vmx_vmcs_write(VMCS_CR4_GUEST_HOST_MASK, cr4_fixed0);
        vmx_vmcs_write(VMCS_CR4_READ_SHADOW, cr4 & ~cr4_fixed0);
    }

    vmx_vmcs_write(VMCS_GUEST_CS_SELECTOR, 0x08);
    vmx_vmcs_write(VMCS_GUEST_SS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_DS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_ES_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_FS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_GS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_LDTR_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_TR_SELECTOR, 0x18);

    vmx_vmcs_write(VMCS_GUEST_CS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_SS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_DS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_ES_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_FS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_GS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_LDTR_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_TR_BASE, gi->gdt_gpa + 0x100);
    vmx_vmcs_write(VMCS_GUEST_GDTR_BASE, gi->gdt_gpa);
    vmx_vmcs_write(VMCS_GUEST_IDTR_BASE, 0);

    vmx_vmcs_write(VMCS_GUEST_CS_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_SS_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_DS_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_ES_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_FS_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_GS_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_LDTR_LIMIT, 0xFFFF);
    vmx_vmcs_write(VMCS_GUEST_TR_LIMIT, 0x67);
    vmx_vmcs_write(VMCS_GUEST_GDTR_LIMIT, 0x28);
    vmx_vmcs_write(VMCS_GUEST_IDTR_LIMIT, 0xFFFF);

    vmx_vmcs_write(VMCS_GUEST_CS_ACCESS, 0xA09B);
    vmx_vmcs_write(VMCS_GUEST_SS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_DS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_ES_ACCESS, 0xC093);
    /* selector=0 的段必须标 unusable（bit16），否则 VM-entry 校验失败 */
    vmx_vmcs_write(VMCS_GUEST_FS_ACCESS, 0x1C093);
    vmx_vmcs_write(VMCS_GUEST_GS_ACCESS, 0x1C093);
    vmx_vmcs_write(VMCS_GUEST_LDTR_ACCESS, 0x10082);
    vmx_vmcs_write(VMCS_GUEST_TR_ACCESS, 0x8B);

    vmx_vmcs_write(VMCS_GUEST_RIP, gi->kernel_entry);
    vmx_vmcs_write(VMCS_GUEST_RSP, ZT5_BOOT_STACK_TOP);
    vmx_vmcs_write(VMCS_GUEST_RFLAGS, 0x2);

    vmx_vmcs_write(VMCS_GUEST_IA32_EFER, EFER_LME | EFER_LMA | EFER_NXE);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_CS, 0);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_ESP, 0);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_EIP, 0);
    vmx_vmcs_write(VMCS_GUEST_DR7, 0x400);
    vmx_vmcs_write(VMCS_GUEST_ACTIVITY_STATE, 0);
    vmx_vmcs_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
}

static void zt5_vmcs_setup_controls(u64 eptp) {
    u64 pin_want = PIN_EXT_INTERRUPT_EXITING | PIN_NMI_EXITING;
    if (vmx_preemption_timer_supported()) pin_want |= PIN_VMX_PREEMPTION_TIMER;
    u64 pin = zt5_adjust_control(pin_want, IA32_VMX_TRUE_PINBASED_CTLS);

    u64 cpu = CPU_BASED_HLT_EXITING
            | CPU_BASED_ACTIVATE_SECONDARY
            | CPU_BASED_USE_MSR_BITMAPS
            | CPU_BASED_UNCOND_IO_EXITING
            | CPU_BASED_INVLPG_EXITING;
    cpu = zt5_adjust_control(cpu, IA32_VMX_TRUE_PROCBASED_CTLS);

    u64 cpu2 = SEC_EXEC_ENABLE_EPT | SEC_EXEC_ENABLE_VPID;
    {
        u64 msr = vmx_read_msr(IA32_VMX_PROCBASED_CTLS2);
        cpu2 &= (u32)(msr >> 32);
        cpu2 |= SEC_EXEC_ENABLE_EPT;
    }

    u64 exit_ctrl = VM_EXIT_SAVE_DEBUG_CONTROLS | VM_EXIT_HOST_ADDR_SPACE_SIZE | VM_EXIT_LOAD_HOST_EFER;
    exit_ctrl = zt5_adjust_control(exit_ctrl, IA32_VMX_TRUE_EXIT_CTLS);
    u64 entry_ctrl = VM_ENTRY_LOAD_DEBUG_CONTROLS | VM_ENTRY_IA32E_MODE_GUEST | VM_ENTRY_LOAD_GUEST_EFER;
    entry_ctrl = zt5_adjust_control(entry_ctrl, IA32_VMX_TRUE_ENTRY_CTLS);

    vmx_vmcs_write(VMCS_PIN_BASED_VM_EXEC_CONTROL, pin);
    vmx_vmcs_write(VMCS_CPU_BASED_VM_EXEC_CONTROL, cpu);
    vmx_vmcs_write(VMCS_SECONDARY_VM_EXEC_CONTROL, cpu2);
    vmx_vmcs_write(VMCS_VM_EXIT_CONTROLS, exit_ctrl);
    vmx_vmcs_write(VMCS_VM_ENTRY_CONTROLS, entry_ctrl);

    /* MSR bitmap: 拦截 LAPIC 与 APIC_BASE */
    for (u32 m = 0x800; m <= 0x8FF; m++) msr_bitmap_intercept(m);
    msr_bitmap_intercept(0x1B);
    /* 内核镜像是 higher-half（0xffffffff80...），不能用 -HHDM 求物理地址；
     * 必须走 Limine kernel-address 转换（否则 VM-entry invalid guest state）。 */
    u64 msr_bitmap_phys = vmx_kernel_virt_to_phys(g_zt5_msr_bitmap);
    log_hex64("[ZT5] MSR bitmap phys=", msr_bitmap_phys);
    vmx_vmcs_write(VMCS_MSR_BITMAP, msr_bitmap_phys);

    vmx_vmcs_write(VMCS_EPT_POINTER, eptp);
    vmx_vmcs_write(VMCS_VPID, 4);
    vmx_vmcs_write(VMCS_EXCEPTION_BITMAP, (1ULL << 13) | (1ULL << 14) | (1ULL << 6) | (1ULL << 8));
}

int zt5_launch(void) {
    const struct zt5_guest_info *gi = zt5_get_guest_info();
    if (!gi || !gi->loaded) return -1;
    if (!vmm_is_ready()) return -2;

    log_info("[ZT5] launch begin");
    vmm_set_active_profile(VM_PROFILE_ZT5);
    /* P8.4：VMM self-test / 前一 guest 可能留下 launched VMCS，先 VMCLEAR。 */
    if (vmx_vmcs_clear(vmm_get_vmcs_phys()) != 0) {
        log_error("[ZT5] vmcs clear failed");
        return -4;
    }
    if (vmx_vmcs_load(vmm_get_vmcs_phys()) != 0) {
        log_error("[ZT5] vmptrld after vmclear failed");
        return -5;
    }
    ept_flush_ept();
    g_guest_terminated = 0;
    g_last_exit_reason = 0xFFFFFFFFULL;
    vm_machine_set_active_state(VM_MACHINE_RUNNING);

    zt5_vmcs_setup_guest_state(gi);
    zt5_vmcs_setup_controls(vmm_get_eptp());
    zt5_vmcs_setup_host_state();
    vmx_vmcs_write(VMCS_GUEST_PREEMPTION_TIMER, vmx_preemption_quantum());

    log_hex64("[ZT5] RIP=", gi->kernel_entry);
    log_hex64("[ZT5] RSP=", ZT5_BOOT_STACK_TOP);
    log_hex64("[ZT5] CR3=", gi->pgt_gpa);
    log_info("[ZT5] vmlaunch (host suspended until guest terminates)");

    int failed;
    __asm__ volatile(
        "movq g_active_saved_host_rsp(%%rip), %%rax\n\t"
        "movq %%rsp, (%%rax)\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq g_active_saved_return_rip(%%rip), %%rcx\n\t"
        "movq %%rax, (%%rcx)\n\t"
        "vmlaunch\n\t"
        "movl $1, %0\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "movl $0, %0\n\t"
        "2:\n\t"
        : "=r"(failed)
        :: "rax", "rcx", "rdx", "rsi", "rdi",
           "r8", "r9", "r10", "r11", "memory"
    );

    if (failed) {
        u64 error = vmx_vmcs_read(VMCS_VMX_INSTRUCTION_ERROR);
        log_hex64("[ZT5] vmlaunch failed, error=", error);
        vm_machine_set_active_state(VM_MACHINE_STOPPED);
        return -3;
    }
    vm_machine_set_active_state(VM_MACHINE_STOPPED);
    log_info("[ZT5] guest terminated, host resumed");
    log_hex64("[ZT5] last exit reason=", g_last_exit_reason);
    log_hex64("[ZT5] vmexit count=", vmexit_get_count());
    return 0;
}
