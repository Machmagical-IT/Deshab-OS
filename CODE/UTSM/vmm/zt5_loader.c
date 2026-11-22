/* zt5_loader.c — ZT5 (ZratroOS) kernel as a UTSM VMX non-root guest.
 *
 * 流程:
 *   1. 在 boot module 中查找 ZT5 内核（cmdline "zt5:kernel" 或路径含 zt5）
 *   2. 解析 ELF64 ET_EXEC（fixed link 0xFFFFFFFF80000000），装载 PT_LOAD
 *   3. 分配 boot area / framebuffer / 模块文件区 / guest RAM
 *   4. 构造 guest 页表（恒等 0-4GB + HHDM + 内核高半区）
 *   5. 构造伪 Limine 响应（hhdm/memmap/fb/kernel_file/module/cmdline）
 *   6. 扫描内核 .limine_reqs 段回填 response 指针
 *   7. EPT 映射全部 guest 区域
 *
 * VMCS 与 vmlaunch 见 zt5_boot.c。
 */

#include <utsm/zt5_loader.h>
#include <utsm/panic.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/dma.h>
#include <utsm/paging.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_module_request g_module_request;
extern volatile struct limine_hhdm_request g_hhdm_request;

/* ===== 基础 ===== */
static void mem_copy(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
}
static void mem_zero(void *dst, u64 n) {
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++) d[i] = 0;
}
static int str_starts(const char *s, const char *p) {
    while (*p) { if (!s || *s++ != *p++) return 0; }
    return 1;
}

/* ===== ELF64 ===== */
#define ELF_MAGIC      0x464C457FULL
#define ELF_ET_EXEC    2
#define ELF_PT_LOAD    1
#define ELF_SHT_PROGBITS 1
#define ELF_SHT_NOBITS   8

typedef struct __attribute__((packed)) {
    u8 ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} zt5_ehdr;

typedef struct __attribute__((packed)) {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} zt5_phdr;

typedef struct __attribute__((packed)) {
    u32 name, type;
    u64 flags, addr, offset, size;
    u32 link, info;
    u64 addralign, entsize;
} zt5_shdr;

static int zt5_parse_elf(const void *img, u64 size, u64 *entry_out, u64 *span_out) {
    if (size < sizeof(zt5_ehdr)) return -1;
    const zt5_ehdr *eh = (const zt5_ehdr *)img;
    if (*(const u32 *)eh->ident != (u32)ELF_MAGIC) return -2;
    if (eh->ident[4] != 2 || eh->type != ELF_ET_EXEC) return -2;
    if (eh->phnum == 0 || eh->phentsize != sizeof(zt5_phdr) ||
        eh->phoff + (u64)eh->phnum * sizeof(zt5_phdr) > size) return -2;
    u64 span = 0;
    const zt5_phdr *ph = (const zt5_phdr *)((const u8 *)img + eh->phoff);
    for (u16 i = 0; i < eh->phnum; i++) {
        if (ph[i].type != ELF_PT_LOAD) continue;
        if (ph[i].vaddr < ZT5_KERNEL_VADDR) return -3;
        u64 off = ph[i].vaddr - ZT5_KERNEL_VADDR;
        if (off + ph[i].memsz > ZT5_GUEST_KERNEL_MAX) return -4;
        if (ph[i].offset + ph[i].filesz > size) return -5;
        if (off + ph[i].memsz > span) span = off + ph[i].memsz;
    }
    if (!span) return -6;
    *entry_out = eh->entry;
    *span_out = span;
    return 0;
}

/* ===== Guest 页表 ===== */
#define ZT5_GDT_OFF   0x0000
#define ZT5_PGT_OFF   0x1000
#define ZT5_STACK_OFF 0x9000
#define ZT5_STACK_SIZE 0x4000
#define ZT5_DATA_OFF  0xD000

#define ZT5_PGT_GPA   (ZT5_BOOT_AREA_GPA + ZT5_PGT_OFF)
#define ZT5_GDT_GPA   (ZT5_BOOT_AREA_GPA + ZT5_GDT_OFF)
#define ZT5_STACK_TOP (ZT5_BOOT_AREA_GPA + ZT5_STACK_OFF + ZT5_STACK_SIZE)

#define PTE_P  (1ULL << 0)
#define PTE_RW (1ULL << 1)
#define PTE_PS (1ULL << 7)

static void zt5_build_page_tables(u8 *boot, u64 kernel_gpa, u64 kernel_span) {
    u8 *base = boot + ZT5_PGT_OFF;
    mem_zero(base, 8 * 4096);
    u64 *pml4 = (u64 *)base;
    u64 *pdpt0 = (u64 *)(base + 0x1000);
    u64 *pd0 = (u64 *)(base + 0x2000);
    u64 *pd1 = (u64 *)(base + 0x3000);
    u64 *pd2 = (u64 *)(base + 0x4000);
    u64 *pd3 = (u64 *)(base + 0x5000);
    u64 *pdptk = (u64 *)(base + 0x6000);
    u64 *pdk = (u64 *)(base + 0x7000);

    u64 pml4_gpa = ZT5_PGT_GPA;
    pml4[0] = (pml4_gpa + 0x1000) | PTE_P | PTE_RW;
    pml4[256] = (pml4_gpa + 0x1000) | PTE_P | PTE_RW;
    pml4[511] = (pml4_gpa + 0x6000) | PTE_P | PTE_RW;

    u64 *pds[4] = { pd0, pd1, pd2, pd3 };
    for (int i = 0; i < 4; i++) {
        pdpt0[i] = (pml4_gpa + 0x2000 + (u64)i * 0x1000) | PTE_P | PTE_RW;
        for (int j = 0; j < 512; j++) {
            u64 gpa = (u64)i * (1ULL << 30) + (u64)j * (2ULL << 20);
            pds[i][j] = gpa | PTE_P | PTE_RW | PTE_PS;
        }
    }

    /* 0xffffffff80000000 的 PDPT 索引 = bits[38:30] = 510（xj380 的 480
     * 是 0xffffff8000000000 的索引，不适用于 ZT5 的 0xffffffff80000000）。 */
    pdptk[510] = (pml4_gpa + 0x7000) | PTE_P | PTE_RW;
    u64 pages = (kernel_span + (2ULL << 20) - 1) >> 21;
    for (u64 i = 0; i < pages; i++) {
        pdk[i] = (kernel_gpa + (i << 21)) | PTE_P | PTE_RW | PTE_PS;
    }
}

/* ===== GDT ===== */
typedef struct __attribute__((packed)) {
    u16 limit_low, base_low;
    u8 base_mid, access, flags_limit_high, base_high;
} zt5_gdt_entry;
typedef struct __attribute__((packed)) {
    u32 limit_low_base_low;
    u8 base_mid, access, flags_limit_high, base_high;
    u32 base_upper, reserved;
} zt5_tss_desc;

static void zt5_build_gdt(u8 *boot) {
    u8 *pg = boot + ZT5_GDT_OFF;
    mem_zero(pg, 4096);
    zt5_gdt_entry *g = (zt5_gdt_entry *)pg;
    g[1].limit_low = 0xFFFF; g[1].access = 0x9B; g[1].flags_limit_high = 0xAF;
    g[2].limit_low = 0xFFFF; g[2].access = 0x93; g[2].flags_limit_high = 0xCF;
    zt5_tss_desc *t = (zt5_tss_desc *)&pg[0x18];
    u64 tss_base = ZT5_GDT_GPA + 0x100;
    u32 tss_limit = 0x67;
    t->limit_low_base_low = (tss_limit & 0xFFFF) | ((u32)(tss_base & 0xFFFFFF) << 16);
    t->base_mid = (u8)((tss_base >> 24) & 0xFF);
    t->access = 0x89;
    t->flags_limit_high = (u8)((tss_limit >> 16) & 0x0F);
}

/* ===== Limine 结构（与 ZT5 include/limine.h 布局一致） ===== */
typedef struct {
    u64 revision;
    void *address;
    u64 size;
    char *path;
    char *cmdline;
    u32 media_type, unused, tftp_ip, tftp_port, partition_index, mbr_disk_id;
    u8 gpt_disk_uuid[16], gpt_part_uuid[16], part_uuid[16];
} zt5_limine_file;

typedef struct __attribute__((packed)) {
    u64 base, length, type;
} zt5_memmap_entry;

typedef struct {
    u64 revision;
    u64 entry_count;
    zt5_memmap_entry **entries;
} zt5_memmap_response;

typedef struct {
    u64 revision;
    u64 offset;
} zt5_hhdm_response;

typedef struct {
    void *address;
    u64 width, height, pitch;
    u16 bpp;
    u8 memory_model, red_mask_size, red_mask_shift, green_mask_size,
       green_mask_shift, blue_mask_size, blue_mask_shift;
    u8 unused[7];
    u64 edid_size;
    void *edid;
    u64 mode_count;
    void **modes;
} zt5_framebuffer;

typedef struct {
    u64 revision;
    u64 framebuffer_count;
    zt5_framebuffer **framebuffers;
} zt5_fb_response;

typedef struct {
    u64 revision;
    zt5_limine_file *kernel_file;
} zt5_kernel_file_response;

typedef struct {
    u64 revision;
    u64 module_count;
    zt5_limine_file **modules;
} zt5_module_response;

typedef struct {
    u64 revision;
    char *cmdline;
} zt5_cmdline_response;

/* boot data 分配器 */
static u8 *g_data;
static u32 g_data_used;
static void *data_alloc(u64 size, u64 align) {
    u32 off = (g_data_used + (u32)align - 1) & ~((u32)align - 1);
    if (off + size > ZT5_BOOT_AREA_SIZE - ZT5_DATA_OFF) return 0;
    void *p = g_data + off;
    g_data_used = (u32)(off + size);
    mem_zero(p, size);
    return p;
}
static u64 data_gpa(const void *p) {
    return ZT5_BOOT_AREA_GPA + ZT5_DATA_OFF + (u64)((const u8 *)p - g_data);
}
/* 写入 guest 可见结构的指针必须是 **guest HHDM VA**（GPA + 0xFFFF800000000000），
 * 不是 UTSM host VA（host HHDM 指向 boot_buf 的 host 物理地址，两者不同）。 */
static u64 data_hhdm(const void *p) { return data_gpa(p) + ZT5_HHDM_OFFSET; }

/* ===== Limine request 回填 ===== */
static const u64 ID_HHDM[4]  = { 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                 0x48dcf1cb8ad2b852ULL, 0x63984e959a98244bULL };
static const u64 ID_FB[4]    = { 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                 0x9d5827dcd881dd75ULL, 0xa3148604f6fab11bULL };
static const u64 ID_MEMMAP[4]= { 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                 0x67cf3d9d378a806fULL, 0xe304acdfc50c3c62ULL };
static const u64 ID_KFILE[4] = { 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                 0xad97e90e83f1ed67ULL, 0x31eb5d1c5ff23b69ULL };
static const u64 ID_MODULE[4]= { 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                 0x3e7e279702be32afULL, 0xca1c4f3bd1280ceeULL };
static const u64 ID_CMDLINE[4]={ 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                 0x4b161536e598651eULL, 0xb390ad4a2f1f303aULL };

static int id_eq(const u8 *p, const u64 id[4]) {
    const u64 *q = (const u64 *)p;
    return q[0] == id[0] && q[1] == id[1] && q[2] == id[2] && q[3] == id[3];
}

static int patch_range(u8 *base, u64 size,
                       u64 hhdm_rsp, u64 fb_rsp, u64 memmap_rsp,
                       u64 kfile_rsp, u64 mod_rsp, u64 cmdline_rsp) {
    int patched = 0;
    for (u64 off = 0; off + 48 <= size; off += 8) {
        u8 *p = base + off;
        u64 rsp = 0;
        if (id_eq(p, ID_HHDM)) rsp = hhdm_rsp;
        else if (id_eq(p, ID_FB)) rsp = fb_rsp;
        else if (id_eq(p, ID_MEMMAP)) rsp = memmap_rsp;
        else if (id_eq(p, ID_KFILE)) rsp = kfile_rsp;
        else if (id_eq(p, ID_MODULE)) rsp = mod_rsp;
        else if (id_eq(p, ID_CMDLINE)) rsp = cmdline_rsp;
        else continue;
        *(u64 *)(p + 40) = rsp;   /* struct { id[4]; revision; response*; } */
        patched++;
    }
    return patched;
}

static void patch_requests(u8 *kernel, u64 image_size,
                           u64 hhdm_rsp, u64 fb_rsp, u64 memmap_rsp,
                           u64 kfile_rsp, u64 mod_rsp, u64 cmdline_rsp) {
    const zt5_ehdr *eh = (const zt5_ehdr *)kernel;
    int patched = 0;
    int have_sh = (eh->shoff != 0 && eh->shentsize == sizeof(zt5_shdr) && eh->shnum &&
                   eh->shoff + (u64)eh->shnum * sizeof(zt5_shdr) <= image_size);
    const zt5_shdr *sh = have_sh ? (const zt5_shdr *)(kernel + eh->shoff) : 0;
    const char *shstr = 0;
    if (have_sh && eh->shstrndx < eh->shnum) {
        const zt5_shdr *s = &sh[eh->shstrndx];
        if (s->offset + s->size <= image_size) shstr = (const char *)(kernel + s->offset);
    }

    if (shstr) {
        for (u16 i = 1; i < eh->shnum; i++) {
            const char *nm = shstr + sh[i].name;
            if (!str_starts(nm, ".limine_req")) continue;
            if (sh[i].offset + sh[i].size > image_size) continue;
            patched += patch_range(kernel + sh[i].offset, sh[i].size,
                                   hhdm_rsp, fb_rsp, memmap_rsp,
                                   kfile_rsp, mod_rsp, cmdline_rsp);
        }
    }
    if (!patched) {
        /* knl.bin 可能被 strip 掉 section 头：全镜像扫描 ID 序列。
         * 每个 request 是 {u64 id[4]; u64 revision; void *response;}。 */
        log_warn("[ZT5] no .limine_reqs section; scanning image header");
        u64 scan = image_size < 65536 ? image_size : 65536;
        patched = patch_range(kernel, scan,
                              hhdm_rsp, fb_rsp, memmap_rsp,
                              kfile_rsp, mod_rsp, cmdline_rsp);
    }
    log_hex64("[ZT5] limine requests patched=", (u64)patched);
}

/* ===== 模块收集 ===== */
struct zt5_mod_src {
    const char *cmd;     /* "zt5:..." */
    const u8 *data;
    u64 size;
};

static u32 collect_modules(struct zt5_mod_src *out, u32 max) {
    if (!g_module_request.response) return 0;
    struct limine_module_response *rsp = g_module_request.response;
    u32 n = 0;
    for (u64 i = 0; i < rsp->module_count && n < max; i++) {
        struct limine_file *f = rsp->modules[i];
        if (!f || !f->cmdline || !f->address) continue;
        if (!str_starts(f->cmdline, "zt5:")) continue;
        if (str_starts(f->cmdline, "zt5:kernel")) continue;
        out[n].cmd = f->cmdline;
        out[n].data = (const u8 *)f->address;
        out[n].size = f->size;
        n++;
    }
    return n;
}

/* .zts 路径映射：宿主 /zts/x.zts → guest /zrr64/zts/p.zts6/x.zts */
static void guest_module_path(const char *cmd, char *out, u32 cap) {
    const char *p = cmd + 4;   /* skip "zt5:" */
    if (str_starts(p, "/zts/")) {
        const char *bn = p + 5;
        u32 n = 0;
        const char *prefix = "/zrr64/zts/p.zts6/";
        while (prefix[n] && n + 1 < cap) { out[n] = prefix[n]; n++; }
        for (u32 i = 0; bn[i] && n + 1 < cap; i++) out[n++] = bn[i];
        out[n] = 0;
    } else {
        u32 i = 0;
        while (p[i] && i + 1 < cap) { out[i] = p[i]; i++; }
        out[i] = 0;
    }
}

/* ===== 主加载入口 ===== */
static struct zt5_guest_info g_guest;

void *zt5_find_kernel_module(u64 *size_out) {
    if (!g_module_request.response) return 0;
    struct limine_module_response *rsp = g_module_request.response;
    for (u64 i = 0; i < rsp->module_count; i++) {
        struct limine_file *f = rsp->modules[i];
        if (!f) continue;
        int match = 0;
        if (f->cmdline && str_starts(f->cmdline, "zt5:kernel")) match = 1;
        if (!match && f->path) {
            const char *p = f->path;
            for (const char *s = p; *s; s++) {
                if ((s[0] == 'z' || s[0] == 'Z') && (s[1] == 't' || s[1] == 'T') &&
                    (s[2] == '5') && (s[3] == '.' || s[3] == '/')) { match = 1; break; }
            }
        }
        if (!match) continue;
        if (size_out) *size_out = f->size;
        log_info("[ZT5] found kernel module:");
        if (f->path) log_info(f->path);
        log_hex64("[ZT5] module size=", f->size);
        return f->address;
    }
    return 0;
}

int zt5_loader_init(void) {
    log_info("[ZT5] loader init begin");

    u64 krl_size = 0;
    void *krl = zt5_find_kernel_module(&krl_size);
    if (!krl) {
        log_warn("[ZT5] no zt5 kernel boot module");
        return -1;
    }

    u64 entry = 0, span = 0;
    int prc = zt5_parse_elf(krl, krl_size, &entry, &span);
    if (prc != 0) {
        log_hex64("[ZT5] parse rc=", (u64)(i64)prc);
        panic_full("ZT5-E01 KERNEL ELF INVALID", "zt5 kernel image rejected by elf parser", 0);
        return -2;
    }
    if (entry < ZT5_KERNEL_VADDR || entry >= ZT5_KERNEL_VADDR + span) {
        panic_full("ZT5-E02 ENTRY OUTSIDE KERNEL", "zt5 entry point outside kernel span", 0);
        return -3;
    }
    log_hex64("[ZT5] entry=", entry);
    log_hex64("[ZT5] span=", span);

    /* boot area: GDT+PGT+stack+data 一整块 */
    dkm_dma_buffer boot_buf;
    if (dma_alloc_pages(ZT5_BOOT_AREA_SIZE / 4096, 4096, 0, &boot_buf) != 0) {
        panic_full("ZT5-E03 ALLOC BOOT AREA FAILED", "dma_alloc_pages failed for zt5 boot area", 0);
        return -4;
    }
    mem_zero(boot_buf.virt, ZT5_BOOT_AREA_SIZE);
    u8 *boot = (u8 *)boot_buf.virt;

    /* kernel image */
    u64 kernel_alloc = (span + 4095) & ~4095ULL;
    dkm_dma_buffer kernel_buf;
    if (dma_alloc_pages(kernel_alloc / 4096, 2ULL * 1024 * 1024, 0, &kernel_buf) != 0) {
        panic_full("ZT5-E04 ALLOC KERNEL FAILED", "dma_alloc_pages failed for zt5 kernel image", 0);
        return -5;
    }
    mem_zero(kernel_buf.virt, kernel_alloc);
    {
        const zt5_ehdr *eh = (const zt5_ehdr *)krl;
        const zt5_phdr *ph = (const zt5_phdr *)((const u8 *)krl + eh->phoff);
        u8 *dst = (u8 *)kernel_buf.virt;
        for (u16 i = 0; i < eh->phnum; i++) {
            if (ph[i].type != ELF_PT_LOAD) continue;
            u64 off = ph[i].vaddr - ZT5_KERNEL_VADDR;
            mem_copy(dst + off, (const u8 *)krl + ph[i].offset, ph[i].filesz);
            if (ph[i].memsz > ph[i].filesz)
                mem_zero(dst + off + ph[i].filesz, ph[i].memsz - ph[i].filesz);
        }
    }

    /* framebuffer */
    dkm_dma_buffer fb_buf;
    if (dma_alloc_pages(ZT5_FB_SIZE / 4096, 4096, 0, &fb_buf) != 0) {
        panic_full("ZT5-E05 ALLOC FB FAILED", "dma_alloc_pages failed for zt5 framebuffer", 0);
        return -6;
    }
    mem_zero(fb_buf.virt, ZT5_FB_SIZE);

    /* modules area */
    dkm_dma_buffer mod_buf;
    if (dma_alloc_pages(ZT5_MODULES_SIZE / 4096, 2ULL * 1024 * 1024, 0, &mod_buf) != 0) {
        panic_full("ZT5-E06 ALLOC MODULES FAILED", "dma_alloc_pages failed for zt5 module area", 0);
        return -7;
    }
    mem_zero(mod_buf.virt, ZT5_MODULES_SIZE);

    /* guest RAM */
    u64 ram_size = ZT5_GUEST_RAM_SIZE;
    dkm_dma_buffer ram_buf;
    while (ram_size >= ZT5_GUEST_RAM_MIN) {
        if (dma_alloc_pages(ram_size / 4096, 2ULL * 1024 * 1024, 0, &ram_buf) == 0) break;
        ram_size >>= 1;
    }
    if (ram_size < ZT5_GUEST_RAM_MIN) {
        panic_full("ZT5-E07 ALLOC GUEST RAM FAILED", "dma_alloc_pages failed for zt5 guest ram", 0);
        return -8;
    }
    log_hex64("[ZT5] ram gpa=", ZT5_GUEST_RAM_GPA);
    log_hex64("[ZT5] ram size=", ram_size);

    /* data 区 + 伪响应 */
    g_data = boot + ZT5_DATA_OFF;
    g_data_used = 0;

    zt5_hhdm_response *hhdm_rsp = (zt5_hhdm_response *)data_alloc(sizeof(*hhdm_rsp), 16);
    hhdm_rsp->revision = 0;
    hhdm_rsp->offset = ZT5_HHDM_OFFSET;

    zt5_memmap_entry *me[4];
    zt5_memmap_entry **me_ptr = (zt5_memmap_entry **)data_alloc(sizeof(void *) * 4, 16);
    for (int i = 0; i < 4; i++) {
        me[i] = (zt5_memmap_entry *)data_alloc(sizeof(zt5_memmap_entry), 16);
        me_ptr[i] = (zt5_memmap_entry *)data_hhdm(me[i]);
    }
    me[0]->base = 0; me[0]->length = 0x100000; me[0]->type = 1;              /* reserved */
    me[1]->base = ZT5_GUEST_RAM_GPA; me[1]->length = ram_size; me[1]->type = 0; /* usable */
    me[2]->base = ZT5_GUEST_KERNEL_GPA; me[2]->length = kernel_alloc;
    me[2]->type = 6;                                                          /* kernel/modules */
    me[3]->base = ZT5_GUEST_FB_GPA; me[3]->length = ZT5_FB_SIZE; me[3]->type = 7;
    zt5_memmap_response *mm_rsp = (zt5_memmap_response *)data_alloc(sizeof(*mm_rsp), 16);
    mm_rsp->revision = 0;
    mm_rsp->entry_count = 4;
    mm_rsp->entries = (zt5_memmap_entry **)data_hhdm(me_ptr);

    zt5_framebuffer *fb = (zt5_framebuffer *)data_alloc(sizeof(*fb), 16);
    fb->address = (void *)(ZT5_GUEST_FB_GPA + ZT5_HHDM_OFFSET);
    fb->width = ZT5_FB_WIDTH;
    fb->height = ZT5_FB_HEIGHT;
    fb->pitch = (u64)ZT5_FB_WIDTH * 4;
    fb->bpp = 32;
    fb->memory_model = 1;
    fb->red_mask_size = 8; fb->red_mask_shift = 16;
    fb->green_mask_size = 8; fb->green_mask_shift = 8;
    fb->blue_mask_size = 8; fb->blue_mask_shift = 0;
    zt5_framebuffer **fb_ptr = (zt5_framebuffer **)data_alloc(sizeof(void *), 16);
    fb_ptr[0] = (zt5_framebuffer *)data_hhdm(fb);
    zt5_fb_response *fb_rsp = (zt5_fb_response *)data_alloc(sizeof(*fb_rsp), 16);
    fb_rsp->revision = 0;
    fb_rsp->framebuffer_count = 1;
    fb_rsp->framebuffers = (zt5_framebuffer **)data_hhdm(fb_ptr);

    zt5_limine_file *kf = (zt5_limine_file *)data_alloc(sizeof(*kf), 16);
    char *kpath = (char *)data_alloc(32, 8);
    mem_copy(kpath, "boot():/boot/zt5.krl", 21);
    kf->revision = 0;
    kf->address = (void *)ZT5_KERNEL_VADDR;
    kf->size = span;
    kf->path = (char *)data_hhdm(kpath);
    kf->cmdline = (char *)data_hhdm(kpath);
    zt5_kernel_file_response *kf_rsp = (zt5_kernel_file_response *)data_alloc(sizeof(*kf_rsp), 16);
    kf_rsp->revision = 0;
    kf_rsp->kernel_file = kf;

    char *cmdline = (char *)data_alloc(64, 8);
    mem_copy(cmdline, "zt5 console=ttyS0", 18);
    zt5_cmdline_response *cmd_rsp = (zt5_cmdline_response *)data_alloc(sizeof(*cmd_rsp), 16);
    cmd_rsp->revision = 0;
    cmd_rsp->cmdline = (char *)data_hhdm(cmdline);

    /* 模块表 */
    struct zt5_mod_src srcs[96];
    u32 src_n = collect_modules(srcs, 96);
    zt5_limine_file **mod_files = (zt5_limine_file **)data_alloc(sizeof(void *) * (src_n ? src_n : 1), 16);
    u64 mod_off = 0;
    for (u32 i = 0; i < src_n; i++) {
        u32 fsize = (u32)srcs[i].size;
        mod_off = (mod_off + 15) & ~15ULL;
        if (mod_off + fsize > ZT5_MODULES_SIZE) {
            log_warn("[ZT5] module area full; remaining modules dropped");
            break;
        }
        mem_copy((u8 *)mod_buf.virt + mod_off, srcs[i].data, fsize);
        zt5_limine_file *lf = (zt5_limine_file *)data_alloc(sizeof(*lf), 16);
        char *path = (char *)data_alloc(128, 8);
        guest_module_path(srcs[i].cmd, path, 128);
        lf->revision = 0;
        lf->address = (void *)(ZT5_MODULES_GPA + mod_off + ZT5_HHDM_OFFSET);
        lf->size = fsize;
        lf->path = (char *)data_hhdm(path);
        lf->cmdline = (char *)data_hhdm(path);
        mod_files[i] = (zt5_limine_file *)data_hhdm(lf);
        mod_off += fsize;
    }
    zt5_module_response *mod_rsp = (zt5_module_response *)data_alloc(sizeof(*mod_rsp), 16);
    mod_rsp->revision = 0;
    mod_rsp->module_count = src_n;
    mod_rsp->modules = (zt5_limine_file **)data_hhdm(mod_files);
    log_hex64("[ZT5] payload modules=", src_n);

    /* 回填内核 .limine_reqs */
    patch_requests((u8 *)kernel_buf.virt, kernel_alloc,
                   data_hhdm(hhdm_rsp), data_hhdm(fb_rsp), data_hhdm(mm_rsp),
                   data_hhdm(kf_rsp), data_hhdm(mod_rsp), data_hhdm(cmd_rsp));

    /* guest 页表/GDT */
    zt5_build_page_tables(boot, ZT5_GUEST_KERNEL_GPA, span);
    zt5_build_gdt(boot);

    /* EPT */
    if (ept_map_range(ZT5_BOOT_AREA_GPA, boot_buf.phys, ZT5_BOOT_AREA_SIZE, EPT_RWX) != 0) {
        panic_full("ZT5-E10 EPT MAP BOOT AREA FAILED", "ept_map_range failed for zt5 boot area", 0);
        return -9;
    }
    if (ept_map_range(ZT5_GUEST_KERNEL_GPA, kernel_buf.phys, kernel_alloc, EPT_RWX) != 0) {
        panic_full("ZT5-E11 EPT MAP KERNEL FAILED", "ept_map_range failed for zt5 kernel", 0);
        return -10;
    }
    if (ept_map_range(ZT5_GUEST_FB_GPA, fb_buf.phys, ZT5_FB_SIZE, EPT_READ | EPT_WRITE) != 0) {
        panic_full("ZT5-E12 EPT MAP FB FAILED", "ept_map_range failed for zt5 framebuffer", 0);
        return -11;
    }
    if (src_n && ept_map_range(ZT5_MODULES_GPA, mod_buf.phys, ZT5_MODULES_SIZE, EPT_RWX) != 0) {
        panic_full("ZT5-E13 EPT MAP MODULES FAILED", "ept_map_range failed for zt5 modules", 0);
        return -12;
    }
    if (ept_map_range(ZT5_GUEST_RAM_GPA, ram_buf.phys, ram_size, EPT_RWX) != 0) {
        panic_full("ZT5-E14 EPT MAP RAM FAILED", "ept_map_range failed for zt5 guest ram", 0);
        return -13;
    }
    /* 不要 identity-map 低 1MB：boot area 就在 0x20000，identity 映射会把
     * guest 页表 GPA 0x21000 覆盖成 host 低内存（首轮 KVM 测试即 #PF
     * instruction-fetch，PML4[511]=0）。ZT5 不依赖低内存。 */

    g_guest.kernel_gpa = ZT5_GUEST_KERNEL_GPA;
    g_guest.kernel_entry = entry;
    g_guest.kernel_size = span;
    g_guest.pgt_gpa = ZT5_PGT_GPA;
    g_guest.gdt_gpa = ZT5_GDT_GPA;
    g_guest.ram_gpa = ZT5_GUEST_RAM_GPA;
    g_guest.ram_size = ram_size;
    g_guest.fb_gpa = ZT5_GUEST_FB_GPA;
    g_guest.modules_gpa = ZT5_MODULES_GPA;
    g_guest.modules_size = mod_off;
    g_guest.module_count = src_n;
    g_guest.loaded = 1;

    log_info("[ZT5] loader init ok");
    log_hex64("[ZT5] pgt gpa=", ZT5_PGT_GPA);
    log_hex64("[ZT5] boot area gpa=", ZT5_BOOT_AREA_GPA);
    return 0;
}

const struct zt5_guest_info *zt5_get_guest_info(void) {
    return g_guest.loaded ? &g_guest : (struct zt5_guest_info *)0;
}

int zt5_status_text(char *buf, u32 cap) {
    if (!buf || !cap) return -1;
    if (!g_guest.loaded) {
        const char *s = "zt5: not loaded\n";
        u32 i = 0;
        while (s[i] && i + 1 < cap) { buf[i] = s[i]; i++; }
        buf[i] = 0;
        return 0;
    }
    /* 简化格式，避免依赖 snprintf */
    const char *pre = "zt5: loaded entry=0x";
    u32 n = 0;
    while (pre[n] && n + 1 < cap) { buf[n] = pre[n]; n++; }
    static const char hx[] = "0123456789abcdef";
    for (int i = 15; i >= 0 && n + 1 < cap; i--) buf[n++] = hx[(g_guest.kernel_entry >> (i * 4)) & 0xF];
    const char *mid = " ram_mb=";
    for (const char *s = mid; *s && n + 1 < cap; s++) buf[n++] = *s;
    u64 mb = g_guest.ram_size / (1024 * 1024);
    char tmp[24]; int ti = 0;
    if (!mb) tmp[ti++] = '0';
    while (mb) { tmp[ti++] = (char)('0' + mb % 10); mb /= 10; }
    while (ti > 0 && n + 1 < cap) buf[n++] = tmp[--ti];
    const char *tail = " modules=";
    for (const char *s = tail; *s && n + 1 < cap; s++) buf[n++] = *s;
    u64 mc = g_guest.module_count;
    ti = 0;
    if (!mc) tmp[ti++] = '0';
    while (mc) { tmp[ti++] = (char)('0' + mc % 10); mc /= 10; }
    while (ti > 0 && n + 1 < cap) buf[n++] = tmp[--ti];
    if (n + 1 < cap) buf[n++] = '\n';
    buf[n] = 0;
    return 0;
}
