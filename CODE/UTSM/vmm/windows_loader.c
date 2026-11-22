#include <utsm/windows_guest.h>
#include <utsm/vmm.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/dma.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_module_request g_module_request;

static struct windows_guest_info g_windows;

static int str_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a++ != *b++) return 0;
    }
    return *a == 0 && *b == 0;
}

static void copy_bytes(void *dst, const void *src, u64 size) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < size; i++) d[i] = s[i];
}

static struct limine_file *find_module(const char *cmdline) {
    if (!g_module_request.response) return (struct limine_file *)0;
    struct limine_module_response *response = g_module_request.response;
    for (u64 i = 0; i < response->module_count; i++) {
        struct limine_file *file = response->modules[i];
        if (file && str_equal(file->cmdline, cmdline)) return file;
    }
    return (struct limine_file *)0;
}

int windows_guest_prepare(void) {
    if (g_windows.loaded) return 0;
    if (!vmm_is_ready()) {
        panic_full("WIN-E10 VMM NOT READY",
                   "windows guest preparation requires VMX root mode", 0);
        return -1;
    }

    struct limine_file *code = find_module("windows:ovmf-code");
    struct limine_file *vars = find_module("windows:ovmf-vars");
    struct limine_file *esp = find_module("windows:esp");
    if (!code || !vars || !esp) {
        panic_full("WIN-E11 WINDOWS BOOT MODULE MISSING",
                   "OVMF CODE VARS or memory ESP boot module is missing", 0);
        return -2;
    }
    if (!code->size || !vars->size ||
        code->size + vars->size != WINDOWS_PFLASH_SIZE) {
        panic_full("WIN-E12 INVALID OVMF FLASH LAYOUT",
                   "OVMF CODE and VARS must form one 4 MiB pflash window", 0);
        return -3;
    }

    struct vm_machine *machine =
        vm_machine_create("windows11", VM_PROFILE_WINDOWS, WINDOWS_VCPU_COUNT);
    if (!machine || vmm_machine_prepare(machine) != 0 ||
        vmm_machine_activate(machine, 0) != 0) {
        panic_full("WIN-E13 WINDOWS MACHINE CREATE FAILED",
                   "could not prepare the two-vcpu Windows VM", 0);
        return -4;
    }

    dkm_dma_buffer flash;
    if (dma_alloc_pages(WINDOWS_PFLASH_SIZE / 4096, 4096,
                        0x100000000ULL, &flash) != 0) {
        panic_full("WIN-E14 OVMF FLASH ALLOC FAILED",
                   "could not allocate the four-megabyte OVMF flash window", 0);
        return -5;
    }
    copy_bytes(flash.virt, vars->address, vars->size);
    copy_bytes((u8 *)flash.virt + vars->size, code->address, code->size);

    if (ept_map_range(WINDOWS_PFLASH_BASE, flash.phys, vars->size,
                      EPT_READ | EPT_WRITE) != 0 ||
        ept_map_range(WINDOWS_PFLASH_BASE + vars->size,
                      flash.phys + vars->size, code->size,
                      EPT_READ | EPT_EXECUTE) != 0) {
        panic_full("WIN-E15 OVMF FLASH MAP FAILED",
                   "could not map OVMF pflash into the Windows EPT", 0);
        return -6;
    }

    g_windows.machine = machine;
    g_windows.ovmf_vars_gpa = WINDOWS_PFLASH_BASE;
    g_windows.ovmf_vars_size = vars->size;
    g_windows.ovmf_code_gpa = WINDOWS_PFLASH_BASE + vars->size;
    g_windows.ovmf_code_size = code->size;
    g_windows.esp_image = esp->address;
    g_windows.esp_image_size = esp->size;

    if (windows_platform_init(machine) != 0) {
        panic_full("WIN-E16 PLATFORM INIT FAILED",
                   "q35-like Windows platform initialization failed", 0);
        return -7;
    }

    ept_flush_ept();
    g_windows.loaded = 1;
    log_info("[WIN] OVMF and memory ESP prepared");
    log_hex64("[WIN] OVMF vars size=", vars->size);
    log_hex64("[WIN] OVMF code size=", code->size);
    log_hex64("[WIN] memory ESP size=", esp->size);
    log_hex64("[WIN] vars marker=",
              *(u32 *)((u8 *)vars->address + 0x64));
    return 0;
}

const struct windows_guest_info *windows_guest_get_info(void) {
    return g_windows.loaded ? &g_windows : (const struct windows_guest_info *)0;
}
