#ifndef UTSM_VM_MACHINE_H
#define UTSM_VM_MACHINE_H

#include <utsm/types.h>
#include <utsm/ept.h>

#define VM_MACHINE_MAX       4
#define VM_MACHINE_MAX_VCPUS 8
#define VM_VCPU_HOST_STACK_SIZE 8192

enum vm_profile {
    VM_PROFILE_SELFTEST = 0,
    VM_PROFILE_LINUX,
    VM_PROFILE_XJ380,
    VM_PROFILE_ZT5,
    VM_PROFILE_WINDOWS
};

enum vm_machine_state {
    VM_MACHINE_EMPTY = 0,
    VM_MACHINE_CREATED,
    VM_MACHINE_READY,
    VM_MACHINE_RUNNING,
    VM_MACHINE_PARKED,
    VM_MACHINE_STOPPED
};

enum vm_vcpu_state {
    VM_VCPU_OFFLINE = 0,
    VM_VCPU_RESET,
    VM_VCPU_RUNNABLE,
    VM_VCPU_RUNNING,
    VM_VCPU_HALTED
};

struct vmexit_guest_regs {
    u64 rax;
    u64 rbx;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 rbp;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
};

struct vm_vcpu {
    u32 id;
    u16 vpid;
    u16 apic_id;
    enum vm_vcpu_state state;
    u64 vmcs_phys;
    struct vmexit_guest_regs regs;
    u64 xcr0;
    u8 xsave_area[4096] __attribute__((aligned(64)));
    u8 host_stack[VM_VCPU_HOST_STACK_SIZE] __attribute__((aligned(16)));
    u64 saved_host_rsp;
    u64 saved_return_rip;
    u64 last_exit_reason;
    u64 exit_count;
    int terminated;
    int launch_pending;
};

struct vm_machine {
    u32 id;
    enum vm_profile profile;
    enum vm_machine_state state;
    const char *name;
    u32 vcpu_count;
    struct ept_context ept;
    struct vm_vcpu vcpus[VM_MACHINE_MAX_VCPUS];
};

void vm_machine_registry_init(void);
struct vm_machine *vm_machine_create(const char *name, enum vm_profile profile,
                                     u32 vcpu_count);
int vm_machine_activate(struct vm_machine *machine, u32 vcpu_id);
/* 放弃当前 CPU 对 VMCS 的所有权（BSP → AP 交接前调用）。 */
void vm_machine_release_cpu(void);
struct vm_machine *vm_machine_active(void);
struct vm_machine *vm_machine_find_profile(enum vm_profile profile);
struct vm_vcpu *vm_vcpu_active(void);
struct vm_vcpu *vm_machine_vcpu(struct vm_machine *machine, u32 vcpu_id);
int vm_machine_profile_active(enum vm_profile profile);
enum vm_machine_state vm_machine_active_state(void);
void vm_machine_set_active_state(enum vm_machine_state state);

#endif
