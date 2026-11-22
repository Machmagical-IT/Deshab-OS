#ifndef UTSM_VMM_H
#define UTSM_VMM_H

#include <utsm/types.h>
#include <utsm/vm_machine.h>

/* VMM 顶层 API：协调 VMX/EPT/VMCS/VMEXIT，启动 guest。
 *
 * Phase 1.1（当前）：vmm_init() 完成 VMX root 启用 + EPT 初始化，
 * vmm_self_test() 启动一个最小 guest（HLT 指令）验证 VMEXIT 路径。
 *
 * Phase 1.2+：vmm_launch_linux() 加载并启动 Linux 6.6 LTS guest。
 */

/* ===== Guest register save area =====
 *
 * VMCS does NOT save guest GPRs (RAX-R15) on VM-Exit. The assembly VM-Exit
 * handler (vmexit_asm.S) saves all 16 GPRs into this structure before
 * calling the C dispatcher, and restores them before vmresume.
 *
 * C handlers (e.g. hypercall.c) can read guest arguments from this structure
 * and write return values (e.g. g_guest_regs.rax = retval) which will be
 * loaded into the CPU before vmresume resumes the guest. */
/* Active vCPU guest register save area. vmexit_asm.S dereferences this pointer;
 * C call sites retain the historical g_guest_regs.field spelling. */
extern volatile struct vmexit_guest_regs *g_active_guest_regs;
#define g_guest_regs (*g_active_guest_regs)

/* ===== Active-vCPU VM-Exit state =====
 *
 * Assembly dereferences these active-slot pointers. C keeps the historical
 * spelling as lvalue macros while the storage lives in struct vm_vcpu. */
extern volatile u64 *g_active_saved_host_rsp;
extern volatile u64 *g_active_saved_return_rip;
extern volatile int *g_active_guest_terminated;
extern volatile int *g_active_launch_pending;
extern volatile u64 *g_active_last_exit_reason;
#define g_saved_host_rsp    (*g_active_saved_host_rsp)
#define g_saved_return_rip  (*g_active_saved_return_rip)
#define g_guest_terminated  (*g_active_guest_terminated)
#define g_last_exit_reason  (*g_active_last_exit_reason)

/* ===== VMM API ===== */

/* 初始化 VMM：VMX root 模式 + EPT 表 + 分配 VMCS。
 * 返回 0 成功，负值失败。 */
int vmm_init(void);
int vmm_machine_prepare(struct vm_machine *machine);
int vmm_machine_activate(struct vm_machine *machine, u32 vcpu_id);
void vmm_set_active_profile(enum vm_profile profile);

/* 关闭 VMM：vmxoff。 */
void vmm_shutdown(void);

/* Phase 1.1 自检：启动最小 guest 验证 VMX 路径。
 * guest 代码为内嵌的 HLT 指令，预期触发 HLT VMEXIT。
 * 返回 0 成功。 */
int vmm_self_test(void);

/* VMM 状态查询 */
int vmm_is_ready(void);
u64 vmm_get_eptp(void);
/* P8.5: VMCS 物理地址（linux_launch 前 VMCLEAR+VMPTRLD 用） */
u64 vmm_get_vmcs_phys(void);
/* Active vCPU host stack; VMCS HOST_RSP must use this instead of a singleton. */
u8 *vmm_get_host_stack(void);
u64 vmm_get_host_stack_top(void);

/* Walk the current guest CR3 to translate GVA→GPA. Returns 0 if unmapped. */
u64 vmx_guest_gva_to_gpa(u64 gva);

#endif
