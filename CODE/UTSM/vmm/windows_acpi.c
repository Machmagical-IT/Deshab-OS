#include <utsm/windows_guest.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/dma.h>
#include <utsm/log.h>
#include <utsm/panic.h>

#define ACPI_GPA_BASE 0x000E0000ULL
#define ACPI_BYTES    0x00010000ULL

static void zero_bytes(void *ptr, u64 size) {
    u8 *p = (u8 *)ptr;
    for (u64 i = 0; i < size; i++) p[i] = 0;
}

static void put16(u8 *p, u16 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
}

static void put32(u8 *p, u32 value) {
    for (u32 i = 0; i < 4; i++) p[i] = (u8)(value >> (i * 8));
}

static void put64(u8 *p, u64 value) {
    for (u32 i = 0; i < 8; i++) p[i] = (u8)(value >> (i * 8));
}

static void copy_ascii(u8 *p, const char *text, u32 size) {
    for (u32 i = 0; i < size; i++) p[i] = text[i] ? (u8)text[i] : (u8)' ';
}

static u8 checksum(const u8 *p, u32 size) {
    u8 sum = 0;
    for (u32 i = 0; i < size; i++) sum = (u8)(sum + p[i]);
    return (u8)(0 - sum);
}

static void acpi_header(u8 *table, const char *signature, u32 length,
                        u8 revision) {
    copy_ascii(table, signature, 4);
    put32(table + 4, length);
    table[8] = revision;
    table[9] = 0;
    copy_ascii(table + 10, "DESHAB", 6);
    copy_ascii(table + 16, "WIN11VM ", 8);
    put32(table + 24, 1);
    copy_ascii(table + 28, "DSHB", 4);
    put32(table + 32, 1);
}

static void finish_table(u8 *table) {
    u32 length = (u32)table[4] | ((u32)table[5] << 8) |
                 ((u32)table[6] << 16) | ((u32)table[7] << 24);
    table[9] = checksum(table, length);
}

static void gas(u8 *p, u8 space, u8 width, u8 access, u64 address) {
    p[0] = space;
    p[1] = width;
    p[2] = 0;
    p[3] = access;
    put64(p + 4, address);
}

static u32 build_dsdt(u8 *table) {
    static const u8 aml[] = {
        0x10, 0x35, 0x5C, '_','S','B','_',
        0x5B, 0x82, 0x2D, 'P','C','I','0',
        0x08, '_','H','I','D', 0x0D, 'P','N','P','0','A','0','8',0,
        0x08, '_','C','I','D', 0x0D, 'P','N','P','0','A','0','3',0,
        0x08, '_','A','D','R', 0x00,
        0x08, '_','U','I','D', 0x00
    };
    u32 length = 36 + sizeof(aml);
    acpi_header(table, "DSDT", length, 2);
    for (u32 i = 0; i < sizeof(aml); i++) table[36 + i] = aml[i];
    finish_table(table);
    return length;
}

int windows_acpi_build(void) {
    dkm_dma_buffer buffer;
    if (dma_alloc_pages(ACPI_BYTES / 4096, 4096,
                        0x100000000ULL, &buffer) != 0) {
        panic_full("WIN-E29 ACPI MEMORY ALLOC FAILED",
                   "could not allocate the fixed low-memory ACPI arena", 0);
        return -1;
    }
    zero_bytes(buffer.virt, ACPI_BYTES);
    u8 *base = (u8 *)buffer.virt;

    u8 *rsdp = base + 0x0000;
    u8 *xsdt = base + 0x0100;
    u8 *fadt = base + 0x0200;
    u8 *madt = base + 0x0400;
    u8 *mcfg = base + 0x0600;
    u8 *hpet = base + 0x0700;
    u8 *dsdt = base + 0x0800;
    build_dsdt(dsdt);

    acpi_header(fadt, "FACP", 276, 6);
    put32(fadt + 40, (u32)(ACPI_GPA_BASE + 0x0800));
    fadt[45] = 1;
    put16(fadt + 46, 9);
    put32(fadt + 56, 0x600);
    put32(fadt + 64, 0x604);
    put32(fadt + 76, 0x608);
    fadt[88] = 4;
    fadt[89] = 2;
    fadt[91] = 4;
    put16(fadt + 113, 3);
    put32(fadt + 116, (1u << 0) | (1u << 4) | (1u << 5) |
                           (1u << 8) | (1u << 10));
    gas(fadt + 120, 1, 8, 1, 0xCF9);
    fadt[132] = 6;
    put64(fadt + 144, ACPI_GPA_BASE + 0x0800);
    gas(fadt + 152, 1, 32, 3, 0x600);
    gas(fadt + 176, 1, 16, 2, 0x604);
    gas(fadt + 208, 1, 32, 3, 0x608);
    finish_table(fadt);

    acpi_header(madt, "APIC", 82, 5);
    put32(madt + 36, (u32)WINDOWS_LAPIC_BASE);
    put32(madt + 40, 1);
    for (u32 cpu = 0; cpu < WINDOWS_VCPU_COUNT; cpu++) {
        u8 *entry = madt + 44 + cpu * 8;
        entry[0] = 0;
        entry[1] = 8;
        entry[2] = (u8)cpu;
        entry[3] = (u8)cpu;
        put32(entry + 4, 1);
    }
    u8 *io = madt + 60;
    io[0] = 1; io[1] = 12; io[2] = 2;
    put32(io + 4, (u32)WINDOWS_IOAPIC_BASE);
    put32(io + 8, 0);
    u8 *iso = madt + 72;
    iso[0] = 2; iso[1] = 10; iso[2] = 0; iso[3] = 0;
    put32(iso + 4, 2);
    put16(iso + 8, 0);
    finish_table(madt);

    acpi_header(mcfg, "MCFG", 60, 1);
    put64(mcfg + 44, WINDOWS_PCIE_ECAM_BASE);
    put16(mcfg + 52, 0);
    mcfg[54] = 0;
    mcfg[55] = 0xFF;
    finish_table(mcfg);

    acpi_header(hpet, "HPET", 56, 1);
    put32(hpet + 36, 0x8086A201u);
    gas(hpet + 40, 0, 64, 4, WINDOWS_HPET_BASE);
    hpet[52] = 0;
    put16(hpet + 53, 0x0080);
    hpet[55] = 0;
    finish_table(hpet);

    acpi_header(xsdt, "XSDT", 36 + 4 * 8, 1);
    put64(xsdt + 36, ACPI_GPA_BASE + 0x0200);
    put64(xsdt + 44, ACPI_GPA_BASE + 0x0400);
    put64(xsdt + 52, ACPI_GPA_BASE + 0x0600);
    put64(xsdt + 60, ACPI_GPA_BASE + 0x0700);
    finish_table(xsdt);

    copy_ascii(rsdp, "RSD PTR ", 8);
    copy_ascii(rsdp + 9, "DESHAB", 6);
    rsdp[15] = 2;
    put32(rsdp + 20, 36);
    put64(rsdp + 24, ACPI_GPA_BASE + 0x0100);
    rsdp[8] = checksum(rsdp, 20);
    rsdp[32] = checksum(rsdp, 36);

    if (ept_map_range(ACPI_GPA_BASE, buffer.phys, ACPI_BYTES,
                      EPT_READ | EPT_WRITE) != 0) {
        panic_full("WIN-E30 ACPI MEMORY MAP FAILED",
                   "could not map ACPI tables into Windows guest memory", 0);
        return -2;
    }
    log_info("[WIN] ACPI RSDP/XSDT/FADT/MADT/MCFG/HPET ready");
    return 0;
}
