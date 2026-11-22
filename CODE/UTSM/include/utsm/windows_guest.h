#ifndef UTSM_WINDOWS_GUEST_H
#define UTSM_WINDOWS_GUEST_H

#include <utsm/types.h>
#include <utsm/vm_machine.h>

#define WINDOWS_VCPU_COUNT       2u
#define WINDOWS_PFLASH_BASE      0xFFC00000ULL
#define WINDOWS_PFLASH_SIZE      0x00400000ULL
#define WINDOWS_PCIE_ECAM_BASE   0xE0000000ULL
#define WINDOWS_PCIE_ECAM_SIZE   0x10000000ULL
#define WINDOWS_IOAPIC_BASE      0xFEC00000ULL
#define WINDOWS_HPET_BASE        0xFED00000ULL
#define WINDOWS_LAPIC_BASE       0xFEE00000ULL

struct windows_guest_info {
    int loaded;
    struct vm_machine *machine;
    u64 ovmf_code_gpa;
    u64 ovmf_code_size;
    u64 ovmf_vars_gpa;
    u64 ovmf_vars_size;
    const void *esp_image;
    u64 esp_image_size;
};

int windows_guest_prepare(void);
const struct windows_guest_info *windows_guest_get_info(void);
int windows_guest_launch(void);

int windows_platform_init(struct vm_machine *machine);
u32 windows_platform_disk_reads(void);
u32 windows_platform_disk_writes(void);
int windows_platform_string_io(u16 port, int is_in, u32 size, u64 count,
                                u64 linear, int down);
int windows_platform_io(u16 port, int is_in, u32 size, u64 value,
                        u64 *result, int *terminate);
int windows_platform_is_mmio(u64 gpa);
u64 windows_platform_mmio_read(u64 gpa, u32 width);
void windows_platform_mmio_write(u64 gpa, u64 value, u32 width);
void windows_platform_tick(void);
void windows_platform_arm_irq(void);
int windows_platform_irq_armed(void);
int windows_platform_irq_allowed(u8 vector);
void windows_platform_irq_accepted(u8 vector);
int windows_platform_irq_allowed(u8 vector);
void windows_platform_irq_accepted(u8 vector);
u64 windows_platform_apic_base_read(void);
void windows_platform_apic_base_write(u64 value);
u64 windows_platform_x2apic_read(u32 msr);
void windows_platform_x2apic_write(u32 msr, u64 value);
int windows_vcpu_start_ap(u8 startup_vector);
int windows_vcpu_schedule_next(void);
int windows_vcpu_on_resume(int from_hlt);
int windows_vcpu_yield(void);
void windows_vcpu_reset_ap(void);

#endif
