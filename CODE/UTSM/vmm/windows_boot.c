#include <utsm/windows_guest.h>
#include <utsm/vmm.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

extern void vmx_vm_exit_handler(void);
extern u64 vmexit_get_count(void);

static u8 g_windows_msr_bitmap[4096] __attribute__((aligned(4096)));
static int g_windows_ap_online;
static int g_windows_ap_parked;
static int g_windows_ap_hlt_seen;
static u64 g_windows_ap_hlt_rip;
static int g_windows_ap_same_hlt;
static int setup_controls(u64 eptp, u16 vpid);

static u64 read_cr0_local(void) {
    u64 value;
    __asm__ volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

static u64 read_cr3_local(void) {
    u64 value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static u64 read_cr4_local(void) {
    u64 value;
    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static u64 read_msr_local(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static u64 adjust_control(u64 value, u32 msr) {
    u64 caps = vmx_read_msr(msr);
    value |= (u32)caps;
    value &= (u32)(caps >> 32);
    return value;
}

static void setup_host_state(void) {
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
    vmx_vmcs_write(VMCS_HOST_GS_BASE, 0);
    {
        struct __attribute__((packed)) { u16 limit; u64 base; } gdtr, idtr;
        __asm__ volatile("sgdt %0" : "=m"(gdtr));
        __asm__ volatile("sidt %0" : "=m"(idtr));
        vmx_vmcs_write(VMCS_HOST_GDTR_BASE, gdtr.base);
        vmx_vmcs_write(VMCS_HOST_IDTR_BASE, idtr.base);
        u16 tr;
        __asm__ volatile("str %0" : "=r"(tr));
        u64 *desc = (u64 *)(gdtr.base + (tr & 0xFFF8));
        u64 lo = desc[0], hi = desc[1];
        u64 base = (lo >> 16) & 0xFFFFFF;
        base |= ((lo >> 32) & 0xFF) << 24;
        base |= (hi & 0xFFFFFFFF) << 32;
        vmx_vmcs_write(VMCS_HOST_TR_SELECTOR, tr);
        vmx_vmcs_write(VMCS_HOST_TR_BASE, base);
    }
    vmx_vmcs_write(VMCS_HOST_SYSENTER_CS, read_msr_local(0x174) & 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_HOST_SYSENTER_ESP, read_msr_local(0x175));
    vmx_vmcs_write(VMCS_HOST_SYSENTER_EIP, read_msr_local(0x176));
    vmx_vmcs_write(VMCS_HOST_IA32_EFER, read_msr_local(0xC0000080));
    vmx_vmcs_write(VMCS_HOST_RSP, vmm_get_host_stack_top());
    vmx_vmcs_write(VMCS_HOST_RIP, (u64)vmx_vm_exit_handler);
}

static void setup_real_mode_state(u16 cs_selector, u64 cs_base, u64 rip) {
    /* Unrestricted guest exempts only CR0.PE/PG. Every other VMX fixed bit,
     * notably CR0.NE and CR4.VMXE, still has to be present at reset. */
    u64 cr0_fixed0 = vmx_read_msr(IA32_VMX_CR0_FIXED0);
    u64 cr0_fixed1 = vmx_read_msr(IA32_VMX_CR0_FIXED1);
    u64 cr4_fixed0 = vmx_read_msr(IA32_VMX_CR4_FIXED0);
    u64 cr4_fixed1 = vmx_read_msr(IA32_VMX_CR4_FIXED1);
    u64 cr0 = (cr0_fixed0 & ~(CR0_PE | CR0_PG)) | CR0_ET;
    u64 cr4 = cr4_fixed0;
    cr0 &= cr0_fixed1;
    cr4 &= cr4_fixed1;
    vmx_vmcs_write(VMCS_GUEST_CR0, cr0);
    vmx_vmcs_write(VMCS_GUEST_CR3, 0);
    vmx_vmcs_write(VMCS_GUEST_CR4, cr4);
    /* Intercept the paging enable so LMA and the IA-32e entry control
     * stay consistent. PE remains guest-owned for the real-mode reset. */
    vmx_vmcs_write(VMCS_CR0_GUEST_HOST_MASK, CR0_PG);
    vmx_vmcs_write(VMCS_CR0_READ_SHADOW, cr0 & ~CR0_PG);
    /* Keep CR4.VMXE owned by the host. OVMF's reset stub writes CR4 without
     * that bit; exposing the write directly clears VMXE and triple-faults. */
    vmx_vmcs_write(VMCS_CR4_GUEST_HOST_MASK, cr4_fixed0);
    vmx_vmcs_write(VMCS_CR4_READ_SHADOW, 0);
    vmx_vmcs_write(VMCS_VM_ENTRY_INTERRUPT_INFO, 0);
    log_hex64("[WIN] reset CR0=", cr0);
    log_hex64("[WIN] reset CR4=", cr4);

    vmx_vmcs_write(VMCS_GUEST_CS_SELECTOR, cs_selector);
    vmx_vmcs_write(VMCS_GUEST_SS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_DS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_ES_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_FS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_GS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_LDTR_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_TR_SELECTOR, 0);

    vmx_vmcs_write(VMCS_GUEST_CS_BASE, cs_base);
    vmx_vmcs_write(VMCS_GUEST_SS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_DS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_ES_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_FS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_GS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_LDTR_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_TR_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_GDTR_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_IDTR_BASE, 0);

    vmx_vmcs_write(VMCS_GUEST_CS_LIMIT, 0xFFFF);
    vmx_vmcs_write(VMCS_GUEST_SS_LIMIT, 0xFFFF);
    vmx_vmcs_write(VMCS_GUEST_DS_LIMIT, 0xFFFF);
    vmx_vmcs_write(VMCS_GUEST_ES_LIMIT, 0xFFFF);
    vmx_vmcs_write(VMCS_GUEST_FS_LIMIT, 0xFFFF);
    vmx_vmcs_write(VMCS_GUEST_GS_LIMIT, 0xFFFF);
    vmx_vmcs_write(VMCS_GUEST_LDTR_LIMIT, 0xFFFF);
    vmx_vmcs_write(VMCS_GUEST_TR_LIMIT, 0xFFFF);
    vmx_vmcs_write(VMCS_GUEST_GDTR_LIMIT, 0xFFFF);
    vmx_vmcs_write(VMCS_GUEST_IDTR_LIMIT, 0xFFFF);

    vmx_vmcs_write(VMCS_GUEST_CS_ACCESS, 0x009B);
    vmx_vmcs_write(VMCS_GUEST_SS_ACCESS, 0x0093);
    vmx_vmcs_write(VMCS_GUEST_DS_ACCESS, 0x0093);
    vmx_vmcs_write(VMCS_GUEST_ES_ACCESS, 0x0093);
    vmx_vmcs_write(VMCS_GUEST_FS_ACCESS, 0x0093);
    vmx_vmcs_write(VMCS_GUEST_GS_ACCESS, 0x0093);
    vmx_vmcs_write(VMCS_GUEST_LDTR_ACCESS, 0x10082);
    vmx_vmcs_write(VMCS_GUEST_TR_ACCESS, 0x008B);

    vmx_vmcs_write(VMCS_GUEST_RIP, rip);
    vmx_vmcs_write(VMCS_GUEST_RSP, 0);
    vmx_vmcs_write(VMCS_GUEST_RFLAGS, 0x2);
    vmx_vmcs_write(VMCS_GUEST_IA32_EFER, 0);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_CS, 0);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_ESP, 0);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_EIP, 0);
    vmx_vmcs_write(VMCS_GUEST_DR7, 0x400);
    vmx_vmcs_write(VMCS_GUEST_ACTIVITY_STATE, 0);
    vmx_vmcs_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
    vmx_vmcs_write(VMCS_GUEST_LINK_POINTER, ~0ULL);
}

int windows_vcpu_start_ap(u8 startup_vector) {
    struct vm_machine *machine = vm_machine_find_profile(VM_PROFILE_WINDOWS);
    if (!machine || machine->vcpu_count < WINDOWS_VCPU_COUNT) {
        panic_full("WIN-E31 AP START WITHOUT MACHINE",
                   "SIPI arrived without a two-vcpu Windows machine", 0);
        return -1;
    }
    struct vm_vcpu *origin = vm_vcpu_active();
    u32 origin_id = origin ? origin->id : 0;
    struct vm_vcpu *bsp = vm_machine_vcpu(machine, 0);
    struct vm_vcpu *ap = vm_machine_vcpu(machine, 1);
    if (!bsp || !ap) return -2;
    /* INIT-SIPI-SIPI sends the startup twice. The AP VMCS can be
     * launched only once; a second VMCLEAR would unlaunch it. */
    if (ap->launch_pending || ap->exit_count) return 0;
    if (vmm_machine_activate(machine, 1) != 0) return -2;
    if (vmx_vmcs_clear(ap->vmcs_phys) != 0 ||
        vmx_vmcs_load(ap->vmcs_phys) != 0) {
        panic_full("WIN-E32 AP VMCS LOAD FAILED",
                   "could not clear and load the Windows AP VMCS", 0);
        return -3;
    }
    setup_real_mode_state((u16)startup_vector << 8,
                          (u64)startup_vector << 12, 0);
    setup_host_state();
    if (setup_controls(vmm_get_eptp(), ap->vpid) != 0) return -4;
    vmx_vmcs_write(VMCS_GUEST_PREEMPTION_TIMER, vmx_preemption_quantum());
    ap->saved_host_rsp = bsp->saved_host_rsp;
    ap->saved_return_rip = bsp->saved_return_rip;
    ap->terminated = 0;
    ap->state = VM_VCPU_RUNNABLE;
    ap->launch_pending = 1;
    /* INIT clears GPRs. Leftover long-mode registers make the 16-bit
     * prologue load SS with the previous 64-bit CS. */
    u64 *regs = (u64 *)&ap->regs;
    for (u32 i = 0; i < sizeof(ap->regs) / sizeof(u64); i++) regs[i] = 0;
    g_windows_ap_online = 1;
    g_windows_ap_parked = 0;
    g_windows_ap_hlt_seen = 0;
    g_windows_ap_hlt_rip = 0;
    g_windows_ap_same_hlt = 0;
    log_hex64("[WIN] AP SIPI vector=", startup_vector);
    log_hex64("[WIN] ap vmcs cs=", vmx_vmcs_read(VMCS_GUEST_CS_SELECTOR));
    log_hex64("[WIN] ap vmcs cr0=", vmx_vmcs_read(VMCS_GUEST_CR0));
    log_hex64("[WIN] ap vmcs efer=", vmx_vmcs_read(VMCS_GUEST_IA32_EFER));
    log_hex64("[WIN] ap vmcs entry=", vmx_vmcs_read(VMCS_VM_ENTRY_CONTROLS));
    if (vmm_machine_activate(machine, origin_id) != 0) return -5;
    {
        u64 gpa = (u64)startup_vector << 12;
        u64 hpa = ept_gpa_to_hpa(gpa);
        u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
        if (hpa && hhdm) {
            u8 *page = (u8 *)(hhdm + hpa);
            u64 word = 0;
            for (int i = 0; i < 8; i++) word |= (u64)page[i] << (i * 8);
            log_hex64("[WIN] ap trampoline=", word);
            word = 0;
            for (int i = 0; i < 8; i++) word |= (u64)page[8 + i] << (i * 8);
            log_hex64("[WIN] ap trampoline+8=", word);
            word = 0;
            for (int i = 0; i < 8; i++) word |= (u64)page[16 + i] << (i * 8);
            log_hex64("[WIN] ap trampoline+16=", word);
            for (u32 i = 0; i + 6 < 0x1000; i++) {
                if (page[i] == 0x0F && page[i + 1] == 0x01) {
                    u64 op = 0;
                    for (int b = 0; b < 8 && i + b < 0x1000; b++)
                        op |= (u64)page[i + b] << (b * 8);
                    log_hex64("[WIN] ap lgdt off=", i);
                    log_hex64("[WIN] ap lgdt bytes=", op);
                    break;
                }
            }
            {
                u64 farptr = 0, far2 = 0;
                for (int b = 0; b < 8; b++) {
                    farptr |= (u64)page[0xC8 + b] << (b * 8);
                    far2 |= (u64)page[0xD0 + b] << (b * 8);
                }
                log_hex64("[WIN] ap exch+c8=", farptr);
                log_hex64("[WIN] ap exch+d0=", far2);
            }
            for (u32 i = 0; i + 4 < 0x1000; i++) {
                u32 imm = (u32)page[i] | ((u32)page[i + 1] << 8) |
                          ((u32)page[i + 2] << 16) | ((u32)page[i + 3] << 24);
                if (imm == 0x0009F009u) {
                    log_hex64("[WIN] ap ptr off=", i);
                    log_hex64("[WIN] ap ptr=", imm);
                    break;
                }
            }
        }
    }
    return 0;
}

void windows_vcpu_reset_ap(void) {
    struct vm_machine *machine = vm_machine_find_profile(VM_PROFILE_WINDOWS);
    struct vm_vcpu *ap = machine ? vm_machine_vcpu(machine, 1) : 0;
    if (!ap) return;
    /* A duplicate INIT while the trampoline is still running must not
     * rebuild the VMCS. The BSP's next WakeUpAP, sent after the AP has
     * parked in CpuSleep, is the one that has to start it again: that
     * path waits until the AP clears WAKEUP_AP_SIGNAL. */
    if (ap->exit_count) {
        if (!g_windows_ap_parked) return;
        log_info("[WIN] AP INIT accepted");
    }
    ap->launch_pending = 0;
    ap->exit_count = 0;
    ap->terminated = 0;
    ap->state = VM_VCPU_RESET;
    g_windows_ap_parked = 0;
    g_windows_ap_hlt_seen = 0;
    g_windows_ap_hlt_rip = 0;
    g_windows_ap_same_hlt = 0;
}

int windows_vcpu_yield(void) {
    if (!g_windows_ap_online) return 0;
    struct vm_vcpu *current = vm_vcpu_active();
    /* Stay on the AP while it is still inside the low startup page.
     * Once it is in the high image, a pause loop may be waiting for the
     * BSP, so the preemption timer has to share the two vCPUs. */
    if (current && current->id == 1 && !g_windows_ap_hlt_seen) {
        u64 rip = vmx_vmcs_read(VMCS_GUEST_RIP);
        if (rip < 0x100000) return 0;
    }
    return windows_vcpu_schedule_next();
}

int windows_vcpu_on_resume(int from_hlt) {
    if (!g_windows_ap_online ||
        !vm_machine_profile_active(VM_PROFILE_WINDOWS)) return 0;
    struct vm_vcpu *current = vm_vcpu_active();
    if (!current) return 0;
    /* The startup page is only valid while the BSP is inside the SIPI
     * wait. Run the AP to its first HLT before the BSP can time out
     * and reuse that page. */
    if (current->id == 1 && from_hlt) {
        g_windows_ap_hlt_seen = 1;
        u64 rflags = vmx_vmcs_read(VMCS_GUEST_RFLAGS);
        u64 rip = vmx_vmcs_read(VMCS_GUEST_RIP);
        if (rip == g_windows_ap_hlt_rip) g_windows_ap_same_hlt++;
        else { g_windows_ap_hlt_rip = rip; g_windows_ap_same_hlt = 1; }
        /* One cli;hlt is a delay inside startup. The idle loop hits the
         * same RIP again; only then is the AP actually parked. */
        if ((rflags & 0x200) || g_windows_ap_same_hlt >= 2) {
            g_windows_ap_parked = 1;
            /* schedule_next marks the outgoing vCPU runnable. Set
             * HALTED afterwards so a cli;hlt AP stays out of the
             * rotation until the next INIT/SIPI. */
            int switched = windows_vcpu_schedule_next();
            if (!(rflags & 0x200))
                current->state = VM_VCPU_HALTED;
            return switched;
        }
        return 0;
    }
    if (current->id == 0 && !g_windows_ap_parked) {
        struct vm_machine *machine = vm_machine_active();
        struct vm_vcpu *ap = machine ? vm_machine_vcpu(machine, 1) : 0;
        /* Switch only after SIPI has programmed a fresh real-mode VMCS.
         * An INIT alone leaves the AP reset; launching it there resumes
         * the previous long-mode CS at the trampoline. */
        if (ap && ap->launch_pending)
            return windows_vcpu_schedule_next();
    }
    /* The BSP's sti;hlt is waiting for the timer. Switching away here
     * delivers that interrupt to the parked AP, which has IF clear. */
    return 0;
}

int windows_vcpu_schedule_next(void) {
    if (!g_windows_ap_online ||
        !vm_machine_profile_active(VM_PROFILE_WINDOWS)) return 0;
    struct vm_machine *machine = vm_machine_active();
    struct vm_vcpu *current = vm_vcpu_active();
    if (!machine || !current) return -1;
    u32 next_id = current->id == 0 ? 1 : 0;
    struct vm_vcpu *next = vm_machine_vcpu(machine, next_id);
    if (!next || (next->state != VM_VCPU_RUNNABLE &&
                  next->state != VM_VCPU_RUNNING)) return 0;
    current->state = VM_VCPU_RUNNABLE;
    if (vmm_machine_activate(machine, next_id) != 0) {
        panic_full("WIN-E33 VCPU SWITCH FAILED",
                   "cooperative Windows vcpu VMCS switch failed", 0);
        return -2;
    }
    next->state = VM_VCPU_RUNNING;
    return 1;
}

static int setup_controls(u64 eptp, u16 vpid) {
    u64 pin = PIN_EXT_INTERRUPT_EXITING | PIN_NMI_EXITING;
    if (vmx_preemption_timer_supported()) pin |= PIN_VMX_PREEMPTION_TIMER;
    pin = adjust_control(pin, IA32_VMX_TRUE_PINBASED_CTLS);

    u64 cpu = CPU_BASED_HLT_EXITING |
              CPU_BASED_ACTIVATE_SECONDARY |
              CPU_BASED_USE_MSR_BITMAPS |
              CPU_BASED_UNCOND_IO_EXITING |
              CPU_BASED_INVLPG_EXITING |
              CPU_BASED_CR8_LOAD_EXITING |
              CPU_BASED_CR8_STORE_EXITING;
    cpu = adjust_control(cpu, IA32_VMX_TRUE_PROCBASED_CTLS);
    if (!(cpu & CPU_BASED_CR8_LOAD_EXITING) ||
        !(cpu & CPU_BASED_CR8_STORE_EXITING)) {
        panic_full("WIN-E38 CR8 EXITING UNAVAILABLE",
                   "Windows TPL uses CR8 and cannot share the host LAPIC", 0);
        return -1;
    }

    u64 caps2 = vmx_read_msr(IA32_VMX_PROCBASED_CTLS2);
    u64 cpu2 = SEC_EXEC_ENABLE_EPT | SEC_EXEC_ENABLE_VPID |
               SEC_EXEC_UNRESTRICTED_GUEST;
    cpu2 &= (u32)(caps2 >> 32);
    if (!(cpu2 & SEC_EXEC_ENABLE_EPT) ||
        !(cpu2 & SEC_EXEC_UNRESTRICTED_GUEST)) {
        panic_full("WIN-E19 UNRESTRICTED GUEST UNAVAILABLE",
                   "OVMF reset mode requires EPT and unrestricted guest", 0);
        return -1;
    }

    u64 exit_ctrl = VM_EXIT_SAVE_DEBUG_CONTROLS |
                    VM_EXIT_HOST_ADDR_SPACE_SIZE |
                    VM_EXIT_LOAD_HOST_EFER;
    exit_ctrl = adjust_control(exit_ctrl, IA32_VMX_TRUE_EXIT_CTLS);
    u64 entry_ctrl = VM_ENTRY_LOAD_DEBUG_CONTROLS |
                     VM_ENTRY_LOAD_GUEST_EFER;
    entry_ctrl = adjust_control(entry_ctrl, IA32_VMX_TRUE_ENTRY_CTLS);
    if (!(entry_ctrl & VM_ENTRY_LOAD_GUEST_EFER)) {
        panic_full("WIN-E35 GUEST EFER LOAD UNAVAILABLE",
                   "real-mode OVMF entry requires loading a zero guest EFER", 0);
        return -1;
    }

    for (u32 i = 0; i < sizeof(g_windows_msr_bitmap); i++)
        g_windows_msr_bitmap[i] = 0xFF;
    vmx_vmcs_write(VMCS_PIN_BASED_VM_EXEC_CONTROL, pin);
    vmx_vmcs_write(VMCS_CPU_BASED_VM_EXEC_CONTROL, cpu);
    vmx_vmcs_write(VMCS_SECONDARY_VM_EXEC_CONTROL, cpu2);
    vmx_vmcs_write(VMCS_VM_EXIT_CONTROLS, exit_ctrl);
    vmx_vmcs_write(VMCS_VM_ENTRY_CONTROLS, entry_ctrl);
    vmx_vmcs_write(VMCS_MSR_BITMAP,
                   vmx_kernel_virt_to_phys(g_windows_msr_bitmap));
    vmx_vmcs_write(VMCS_EPT_POINTER, eptp);
    vmx_vmcs_write(VMCS_VPID, vpid);
    /* Faults OVMF turns into CpuSleep leave FinishedCount at 0, so the
     * BSP waits forever. Intercept the ones the AP startup can hit. */
    vmx_vmcs_write(VMCS_EXCEPTION_BITMAP,
                   (1ULL << 6) | (1ULL << 7) | (1ULL << 10) | (1ULL << 11) |
                   (1ULL << 12) | (1ULL << 13) | (1ULL << 14));
    return 0;
}

int windows_guest_launch(void) {
    const struct windows_guest_info *guest = windows_guest_get_info();
    if (!guest || !guest->loaded || !guest->machine) {
        panic_full("WIN-E20 WINDOWS GUEST NOT PREPARED",
                   "windows_guest_launch called without prepared OVMF", 0);
        return -1;
    }
    if (vmm_machine_activate(guest->machine, 0) != 0) return -2;
    struct vm_vcpu *vcpu = vm_vcpu_active();
    if (!vcpu) return -3;

    if (vmx_vmcs_clear(vcpu->vmcs_phys) != 0 ||
        vmx_vmcs_load(vcpu->vmcs_phys) != 0) {
        panic_full("WIN-E21 WINDOWS VMCS LOAD FAILED",
                   "could not clear and load the Windows BSP VMCS", 0);
        return -4;
    }
    setup_real_mode_state(0xF000, 0xFFFF0000, 0xFFF0);
    setup_host_state();
    if (setup_controls(vmm_get_eptp(), vcpu->vpid) != 0) return -5;
    vmx_vmcs_write(VMCS_GUEST_PREEMPTION_TIMER, vmx_preemption_quantum());

    g_guest_terminated = 0;
    g_last_exit_reason = ~0ULL;
    g_windows_ap_online = 0;
    g_windows_ap_parked = 0;
    g_windows_ap_hlt_seen = 0;
    g_windows_ap_hlt_rip = 0;
    g_windows_ap_same_hlt = 0;
    vmx_guest_irq_reset();
    vm_machine_set_active_state(VM_MACHINE_RUNNING);
    log_info("[WIN] launching OVMF reset vector");

    int failed;
    u64 flags;
    __asm__ volatile(
        "movq g_active_saved_host_rsp(%%rip), %%rax\n\t"
        "movq %%rsp, (%%rax)\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq g_active_saved_return_rip(%%rip), %%rcx\n\t"
        "movq %%rax, (%%rcx)\n\t"
        "vmlaunch\n\t"
        "movl $1, %0\n\t"
        "pushfq\n\t"
        "popq %1\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "movl $0, %0\n\t"
        "xorq %1, %1\n\t"
        "2:\n\t"
        : "=r"(failed), "=r"(flags)
        :
        : "rax", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "memory"
    );

    if (failed) {
        u64 error = 0;
        if (flags & 0x40)
            vmx_vmcs_read_checked(VMCS_VMX_INSTRUCTION_ERROR, &error);
        log_hex64("[WIN] vmlaunch flags=", flags);
        log_hex64("[WIN] vmlaunch error=", error);
        vm_machine_set_active_state(VM_MACHINE_STOPPED);
        panic_full("WIN-E22 OVMF VMLAUNCH FAILED",
                   "Windows OVMF BSP failed VM entry", 0);
        return -6;
    }
    vm_machine_set_active_state(VM_MACHINE_STOPPED);
    log_hex64("[WIN] last exit reason=", g_last_exit_reason);
    log_hex64("[WIN] vmexit count=", vmexit_get_count());
    if (g_last_exit_reason == EXIT_ENTRY_FAIL_GUEST_STATE) {
        log_hex64("[WIN] guest CR0=", vmx_vmcs_read(VMCS_GUEST_CR0));
        log_hex64("[WIN] guest CR4=", vmx_vmcs_read(VMCS_GUEST_CR4));
        log_hex64("[WIN] guest EFER=", vmx_vmcs_read(VMCS_GUEST_IA32_EFER));
        log_hex64("[WIN] guest CS=", vmx_vmcs_read(VMCS_GUEST_CS_SELECTOR));
        log_hex64("[WIN] guest CS base=", vmx_vmcs_read(VMCS_GUEST_CS_BASE));
        log_hex64("[WIN] guest CS access=", vmx_vmcs_read(VMCS_GUEST_CS_ACCESS));
        log_hex64("[WIN] entry controls=",
                  vmx_vmcs_read(VMCS_VM_ENTRY_CONTROLS));
        log_hex64("[WIN] secondary controls=",
                  vmx_vmcs_read(VMCS_SECONDARY_VM_EXEC_CONTROL));
        panic_full("WIN-E36 OVMF ENTRY GUEST STATE",
                   "VM-entry rejected the OVMF reset guest state", 0);
        return -7;
    }
    return 0;
}
