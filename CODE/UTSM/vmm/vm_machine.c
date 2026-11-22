#include <utsm/vm_machine.h>
#include <utsm/panic.h>
#include <utsm/log.h>
#include <utsm/smp.h>
#include <utsm/vmx.h>

static struct vm_machine g_machines[VM_MACHINE_MAX];
static u32 g_machine_count;
static struct vm_machine *g_active_machine;
static struct vm_vcpu *g_active_vcpu;
static u32 g_machine_owner_cpu;
static int g_machine_owner_set;
static struct vmexit_guest_regs g_fallback_regs;
static u64 g_fallback_u64;
static int g_fallback_int;
volatile struct vmexit_guest_regs *g_active_guest_regs = &g_fallback_regs;
volatile u64 *g_active_saved_host_rsp = &g_fallback_u64;
volatile u64 *g_active_saved_return_rip = &g_fallback_u64;
volatile u64 *g_active_last_exit_reason = &g_fallback_u64;
volatile int *g_active_guest_terminated = &g_fallback_int;
volatile int *g_active_launch_pending = &g_fallback_int;

static void zero_bytes(void *ptr, usize size) {
    u8 *p = (u8 *)ptr;
    for (usize i = 0; i < size; i++) p[i] = 0;
}

void vm_machine_registry_init(void) {
    zero_bytes(g_machines, sizeof(g_machines));
    g_machine_count = 0;
    g_active_machine = (struct vm_machine *)0;
    g_active_vcpu = (struct vm_vcpu *)0;
    zero_bytes(&g_fallback_regs, sizeof(g_fallback_regs));
    g_fallback_u64 = 0;
    g_fallback_int = 0;
    g_active_guest_regs = &g_fallback_regs;
    g_active_saved_host_rsp = &g_fallback_u64;
    g_active_saved_return_rip = &g_fallback_u64;
    g_active_last_exit_reason = &g_fallback_u64;
    g_active_guest_terminated = &g_fallback_int;
    g_active_launch_pending = &g_fallback_int;
}

struct vm_machine *vm_machine_create(const char *name, enum vm_profile profile,
                                     u32 vcpu_count) {
    if (!name || vcpu_count == 0 || vcpu_count > VM_MACHINE_MAX_VCPUS) {
        panic_full("VMM-E30 INVALID MACHINE CONTRACT",
                   "vm_machine_create received an invalid name or vcpu count", 0);
        return (struct vm_machine *)0;
    }
    if (g_machine_count >= VM_MACHINE_MAX) {
        panic_full("VMM-E31 MACHINE TABLE FULL",
                   "the static vm machine registry has no free slot", 0);
        return (struct vm_machine *)0;
    }

    struct vm_machine *machine = &g_machines[g_machine_count];
    zero_bytes(machine, sizeof(*machine));
    machine->id = g_machine_count;
    machine->name = name;
    machine->profile = profile;
    machine->state = VM_MACHINE_CREATED;
    machine->vcpu_count = vcpu_count;
    for (u32 i = 0; i < vcpu_count; i++) {
        machine->vcpus[i].id = i;
        machine->vcpus[i].vpid = (u16)(1 + machine->id * VM_MACHINE_MAX_VCPUS + i);
        machine->vcpus[i].apic_id = (u16)i;
        machine->vcpus[i].state = VM_VCPU_RESET;
        machine->vcpus[i].xcr0 = 1;
        machine->vcpus[i].last_exit_reason = ~0ULL;
    }
    g_machine_count++;
    log_info("[VMM] machine created");
    log_hex64("[VMM] machine id=", machine->id);
    log_hex64("[VMM] machine vcpus=", machine->vcpu_count);
    return machine;
}

void vm_machine_release_cpu(void) {
    /* VMPTRLD 不允许同一 VMCS 同时在两颗 CPU 上 current。
     * BSP 自检后 VMCS 仍是 current，必须先 VMCLEAR 再交给 AP。 */
    if (g_active_vcpu && g_active_vcpu->vmcs_phys) {
        (void)vmx_vmcs_clear(g_active_vcpu->vmcs_phys);
    }
    g_machine_owner_set = 0;
}

int vm_machine_activate(struct vm_machine *machine, u32 vcpu_id) {
    if (!machine || machine->state == VM_MACHINE_EMPTY ||
        vcpu_id >= machine->vcpu_count) {
        panic_full("VMM-E32 INVALID VCPU ACTIVATION",
                   "vm_machine_activate received an invalid machine or vcpu", 0);
        return -1;
    }
    /* VMCS 是 CPU 局部状态：同一时刻只允许一颗 CPU 激活 machine。
     * BSP 自检后由 guest_runner 调 vm_machine_release_cpu() 移交 AP。 */
    u32 cpu = smp_current_cpu();
    if (g_machine_owner_set && g_machine_owner_cpu != cpu) {
        panic_full("VMM-E42 VMCS CPU OWNERSHIP VIOLATION",
                   "a CPU tried to activate a VMCS owned by another CPU", 0);
        return -3;
    }
    g_machine_owner_cpu = cpu;
    g_machine_owner_set = 1;
    g_active_machine = machine;
    g_active_vcpu = &machine->vcpus[vcpu_id];
    g_active_guest_regs = &g_active_vcpu->regs;
    g_active_saved_host_rsp = &g_active_vcpu->saved_host_rsp;
    g_active_saved_return_rip = &g_active_vcpu->saved_return_rip;
    g_active_last_exit_reason = &g_active_vcpu->last_exit_reason;
    g_active_guest_terminated = &g_active_vcpu->terminated;
    g_active_launch_pending = &g_active_vcpu->launch_pending;
    if (machine->ept.ready && ept_context_activate(&machine->ept) != 0) {
        return -2;
    }
    /* 把该 vCPU 的 VMCS 挂到本 CPU。AP 交接后若不做这一步，后续
     * VMWRITE 全部落到"无当前 VMCS"而静默丢弃，VMLAUNCH 会跑旧
     * self-test VMCS（RIP=0x1003/IDTR=0x5000）。 */
    if (g_active_vcpu->vmcs_phys && vmx_vmcs_load(g_active_vcpu->vmcs_phys) != 0) {
        return -3;
    }
    return 0;
}

struct vm_machine *vm_machine_active(void) {
    return g_active_machine;
}

struct vm_machine *vm_machine_find_profile(enum vm_profile profile) {
    for (u32 i = 0; i < g_machine_count; i++) {
        if (g_machines[i].state != VM_MACHINE_EMPTY &&
            g_machines[i].profile == profile)
            return &g_machines[i];
    }
    return (struct vm_machine *)0;
}

struct vm_vcpu *vm_vcpu_active(void) {
    return g_active_vcpu;
}

struct vm_vcpu *vm_machine_vcpu(struct vm_machine *machine, u32 vcpu_id) {
    if (!machine || vcpu_id >= machine->vcpu_count) return (struct vm_vcpu *)0;
    return &machine->vcpus[vcpu_id];
}

int vm_machine_profile_active(enum vm_profile profile) {
    return g_active_machine && g_active_machine->profile == profile;
}

enum vm_machine_state vm_machine_active_state(void) {
    return g_active_machine ? g_active_machine->state : VM_MACHINE_EMPTY;
}

void vm_machine_set_active_state(enum vm_machine_state state) {
    if (!g_active_machine) {
        panic_full("VMM-E41 NO ACTIVE MACHINE STATE",
                   "attempted to change machine state without an active machine", 0);
        return;
    }
    g_active_machine->state = state;
    if (g_active_vcpu) {
        if (state == VM_MACHINE_RUNNING)
            g_active_vcpu->state = VM_VCPU_RUNNING;
        else if (state == VM_MACHINE_PARKED)
            g_active_vcpu->state = VM_VCPU_HALTED;
        else if (state == VM_MACHINE_READY)
            g_active_vcpu->state = VM_VCPU_RUNNABLE;
        else if (state == VM_MACHINE_STOPPED)
            g_active_vcpu->state = VM_VCPU_OFFLINE;
    }
}
