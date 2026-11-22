/* linux_resume.c — Linux guest park-and-resume 唤醒实现。
 *
 * linux_resume() 通过 vmresume 唤醒已 HLT park 的 Linux guest。
 * guest daemon 处理 IPC 请求后再次 HLT park，控制权返回 host。
 *
 * 控制流与 linux_launch() 的 vmlaunch 对称：
 *   - 保存 host RSP 和返回 RIP
 *   - 执行 vmresume（成功则进入 guest，不返回）
 *   - guest HLT → VM-Exit → handle_hlt park → g_guest_terminated=1
 *   - vmexit_asm.S 恢复 host RSP，jmp 到返回 RIP
 *   - linux_resume() 继续执行，返回 0
 */

#include <utsm/linux_resume.h>
#include <utsm/linux_xsave.h>
#include <utsm/vmx.h>
#include <utsm/vmm.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include <utsm/types.h>
#include <utsm/lapic_timer.h>

/* Must be RIP-relative, not a stack slot: vmexit_asm restores guest GPRs
 * (including RBP) before jumping here, so "=m"(local) can #PF at CR2≈0. */
int g_linux_resume_failed;

int linux_is_parked(void) {
    struct vm_machine *machine = vm_machine_find_profile(VM_PROFILE_LINUX);
    return machine && machine->state == VM_MACHINE_PARKED;
}

int linux_resume(void) {
    if (!linux_is_parked()) {
        log_warn("[LINUX] resume: guest not parked");
        return -1;
    }
    struct vm_machine *machine = vm_machine_find_profile(VM_PROFILE_LINUX);
    if (!machine || vmm_machine_activate(machine, 0) != 0) {
        panic_full("LNX-E28 PARKED VCPU ACTIVATE FAILED",
                   "could not reactivate the parked Linux vcpu", 0);
        return -4;
    }

    g_guest_terminated = 0;
    vm_machine_set_active_state(VM_MACHINE_RUNNING);
    vmx_linux_timeslice_reset();
    linux_xsave_save_host();
    linux_xsave_load_guest();
    lapic_irq_quiesce(1);   /* guest 运行期屏蔽主机可屏蔽中断，杜绝 ext-int 风暴 */
    vmx_linux_prepare_entry();

    /* vmresume 控制流（与 linux_launch 的 vmlaunch 对称）。
     * clobber 全 GPR：从 park 跳回时 CPU 上是 guest 寄存器。 */
    g_linux_resume_failed = 1;
    __asm__ volatile(
        "pushq %%rbx\n\t"
        "pushq %%rbp\n\t"
        "pushq %%r12\n\t"
        "pushq %%r13\n\t"
        "pushq %%r14\n\t"
        "pushq %%r15\n\t"
        "movq g_active_saved_host_rsp(%%rip), %%rax\n\t"
        "movq %%rsp, (%%rax)\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq g_active_saved_return_rip(%%rip), %%rcx\n\t"
        "movq %%rax, (%%rcx)\n\t"
        "vmresume\n\t"
        "movl $1, g_linux_resume_failed(%%rip)\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "movl $0, g_linux_resume_failed(%%rip)\n\t"
        "2:\n\t"
        "popq %%r15\n\t"
        "popq %%r14\n\t"
        "popq %%r13\n\t"
        "popq %%r12\n\t"
        "popq %%rbp\n\t"
        "popq %%rbx\n\t"
        :
        :
        : "rax", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "cc", "memory"
    );

    /* 回到 host 上下文 */
    vmx_linux_timeslice_disarm();
    lapic_irq_quiesce(0);   /* 恢复主机可屏蔽中断 */
    linux_xsave_save_guest();
    linux_xsave_load_host();

    if (g_linux_resume_failed) {
        vm_machine_set_active_state(VM_MACHINE_STOPPED);
        u64 error = vmx_vmcs_read(VMCS_VMX_INSTRUCTION_ERROR);
        log_hex64("[LINUX] vmresume failed, error=", error);
        return -2;
    }
    if (vm_machine_active_state() != VM_MACHINE_PARKED) {
        vm_machine_set_active_state(VM_MACHINE_STOPPED);
        panic_full("LNX-E27 RESUME WITHOUT PARK",
                   "linux vmresume returned without a park transition", 0);
        return -3;
    }

    return 0;
}
