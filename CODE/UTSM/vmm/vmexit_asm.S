/* VM-Exit assembly entry.
 *
 * On VM-Exit, CPU loads host state from VMCS:
 *   - host CR0/CR3/CR4, segment registers
 *   - host RSP = VMCS_HOST_RSP (top of g_host_stack)
 *   - host RIP = VMCS_HOST_RIP (this function)
 *
 * VMCS does NOT save guest GPRs (RAX-R15). They remain in the CPU registers
 * and must be saved explicitly if the C dispatcher wants to read or modify
 * them (e.g. for VMCALL hypercall arguments and return values).
 *
 * This function:
 *   1. Saves all 16 guest GPRs into active_vcpu->regs
 *   2. Calls vmm_vmexit_entry() (C dispatcher, returns void)
 *      - Internally calls vmexit_dispatch(); sets active_vcpu->terminated
 *   3. Restores all 16 guest GPRs from active_vcpu->regs
 *   4. Checks active_vcpu->terminated:
 *      - 0: executes vmresume (GPRs = guest values, possibly modified by C)
 *           If vmresume fails, falls through to terminate
 *      - 1: restores saved host RSP, jmps to saved return RIP
 *
 * The saved RSP/RIP point into the vmlaunch inline asm block (in vmm_self_test
 * or linux_launch). After the jmp, C flow continues with failed=0.
 */

.section .text
.global vmx_vm_exit_handler
vmx_vm_exit_handler:
    /* Step 1: Save all guest GPRs through g_active_guest_regs.
     * RAX is used as the pointer scratch, so preserve its guest value first. */
    pushq %rax
    movq g_active_guest_regs(%rip), %rax
    movq %rbx,  8(%rax)
    movq %rcx,  16(%rax)
    movq %rdx,  24(%rax)
    movq %rsi,  32(%rax)
    movq %rdi,  40(%rax)
    movq %rbp,  48(%rax)
    movq %r8,   56(%rax)
    movq %r9,   64(%rax)
    movq %r10,  72(%rax)
    movq %r11,  80(%rax)
    movq %r12,  88(%rax)
    movq %r13,  96(%rax)
    movq %r14,  104(%rax)
    movq %r15,  112(%rax)
    movq (%rsp), %rbx
    movq %rbx,  0(%rax)
    addq $8, %rsp

    /* Step 2: Align stack to 16 bytes and call C dispatcher.
     * The host RSP (from VMCS_HOST_RSP) is 16-byte aligned minus 8
     * (for the implicit return address), so we sub 8 for alignment. */
    subq $8, %rsp
    call vmm_vmexit_entry
    addq $8, %rsp

    /* Step 3: Restore all 16 guest GPRs from active_vcpu->regs.
     * C handlers may have modified fields (e.g. rax = hypercall return value). */
    movq g_active_guest_regs(%rip), %rax
    movq  8(%rax), %rbx
    movq  16(%rax), %rcx
    movq  24(%rax), %rdx
    movq  32(%rax), %rsi
    movq  40(%rax), %rdi
    movq  48(%rax), %rbp
    movq  56(%rax), %r8
    movq  64(%rax), %r9
    movq  72(%rax), %r10
    movq  80(%rax), %r11
    movq  88(%rax), %r12
    movq  96(%rax), %r13
    movq  104(%rax), %r14
    movq  112(%rax), %r15
    movq  0(%rax), %rax

    /* Step 4: Check terminate flag without clobbering guest RAX.
     * The old `movl g_guest_terminated, %eax` zeroed every IN/CPUID
     * result and livelocked Linux earlyprintk on COM1 LSR (0x3FD). */
    pushq %rax
    movq g_active_guest_terminated(%rip), %rax
    cmpl $0, (%rax)
    popq %rax
    jnz .Lterminate

    /* Resume guest: GPRs are now restored (with any C modifications).
     * A vCPU that has never been launched (Windows AP after SIPI) must
     * use vmlaunch. vmresume does not return on success. */
    pushq %rax
    movq g_active_launch_pending(%rip), %rax
    cmpl $0, (%rax)
    je .Lvmresume
    movl $0, (%rax)
    popq %rax
    vmlaunch
    jmp .Lvmresume_failed
.Lvmresume:
    popq %rax
    vmresume
.Lvmresume_failed:
    /* If we reach here, vmlaunch/vmresume failed (CF=1 or ZF=1). */
    movq g_active_guest_terminated(%rip), %rax
    movl $1, (%rax)
    /* Fall through to terminate */

.Lterminate:
    /* Terminate: restore saved RSP and jmp to saved return RIP.
     * The saved RSP is the caller's stack pointer before vmlaunch.
     * The saved RIP is the "1:" label in the vmlaunch inline asm,
     * which sets failed=0 and continues C flow. */
    movq g_active_saved_host_rsp(%rip), %rax
    movq (%rax), %rsp
    movq g_active_saved_return_rip(%rip), %rax
    jmpq *(%rax)
