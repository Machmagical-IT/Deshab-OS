/* main.c — Deshab EFI 引导器（BOOTX64.EFI）
 *
 * 目标：把 UTSM 从 Limine 的 boot module 变成引导器直接装载的载荷，
 *       同时保留 UTSM 已有的 Limine 协议入口（无需改动内核）。
 *
 * 流程：
 *   1. GOP 初始化，绘制渐变背景 + 无步骤文字的加载条动画
 *   2. 经 EFI SimpleFileSystem 从 ESP 读取 FUCK / manifest / 全部 .drv /
 *      可选 Linux guest 载荷
 *   3. 解析 utsm.elf（ET_EXEC，静态），按 PT_LOAD 复制到物理内存
 *   4. 自建四级页表：恒等映射 + HHDM（0xffff800000000000）+ 内核高半区
 *   5. 伪造 Limine 响应结构（module/fb/hhdm/rsdp/kernel_address/memmap）
 *   6. ExitBootServices → 切 CR3/GDT/栈 → 回填 Limine request → 跳 UTSM
 *
 * 插桩规约：引导器内任何"必须存在"的东西缺失即串口报错 + 停机，
 * 不静默降级（DESHAB-BOOT-E##）。
 */
#include "efi.h"

/* ===================================================================
 * 0. 基础运行时（无 libc）
 * =================================================================== */
void *memset(void *dst, int v, usize n) {
    u8 *d = (u8 *)dst;
    for (usize i = 0; i < n; i++) d[i] = (u8)v;
    return dst;
}

void *memcpy(void *dst, const void *src, usize n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (usize i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static usize str_len(const char *s) {
    usize n = 0;
    while (s && s[n]) n++;
    return n;
}

static int str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int str_ieq_n(const char *a, const char *b, usize n) {
    for (usize i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return 1;
}

static void str_copy(char *dst, const char *src, usize max) {
    usize i = 0;
    if (!max) return;
    while (src && src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    if (max) dst[i] = 0;
}

static void str_cat(char *dst, const char *src, usize max) {
    usize n = str_len(dst);
    usize i = 0;
    if (n >= max) return;
    while (src[i] && n + i + 1 < max) { dst[n + i] = src[i]; i++; }
    dst[n + i] = 0;
}

/* ===================================================================
 * 1. 串口日志（COM1）——实机无屏调试唯一可靠通道
 * =================================================================== */
#define COM1 0x3F8
static inline void outb(u16 port, u8 v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline u8 inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);   /* 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c) {
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) {}
    outb(COM1, (u8)c);
}

static void serial_str(const char *s) {
    while (s && *s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_hex(u64 v) {
    static const char h[] = "0123456789abcdef";
    char b[19];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) b[2 + i] = h[(v >> (60 - i * 4)) & 0xF];
    b[18] = 0;
    serial_str(b);
}

static void log2(const char *a, const char *b) {
    serial_str(a); if (b) serial_str(b); serial_str("\n");
}

static void log_hex2(const char *a, u64 v) { serial_str(a); serial_hex(v); serial_str("\n"); }

/* ===================================================================
 * 2. 全局状态
 * =================================================================== */
static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_GRAPHICS_OUTPUT_PROTOCOL *GOP;
static EFI_FILE_PROTOCOL *RootFs;
static EFI_HANDLE ImageHandle;

static u8 *g_fb;              /* 帧缓冲（恒等映射物理地址） */
static u64 g_fb_w, g_fb_h, g_fb_pitch;
static u32 g_fb_pixfmt;       /* 0=RGBR, 1=BGRR, 2=bitmask */
static u32 g_fb_rsh, g_fb_gsh, g_fb_bsh;
static u32 g_fb_rsz, g_fb_gsz, g_fb_bsz;

static int g_linux_guest;
static int g_windows_guest;
static int g_vscode_install;
static u32 g_vol_serial;

/* 桌面 sprite buffer 固定位于 0x7000000，到 PE_SURFACE_ADDR(0x7800000)
 * 只有 8MiB。分辨率超过该预算会让桌面双缓冲越界，因此引导器只选择
 * 显存需求 ≤ 8MiB 的 GOP 模式（1280x800 / 1366x768 / 1600x900 / 1600x1200）。 */
#define FB_SPRITE_BUDGET (8ULL * 1024ULL * 1024ULL)
static u32 g_want_w, g_want_h;   /* FUCK [boot] gop_width/gop_height，0=自动 */

/* ===================================================================
 * 3. 内存分配
 * =================================================================== */
static u64 alloc_pages(usize bytes) {
    usize pages = (bytes + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    if (!pages) pages = 1;
    EFI_PHYSICAL_ADDRESS p = 0;
    EFI_STATUS st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &p);
    if (st != EFI_SUCCESS) return 0;
    return (u64)p;
}

static void *alloc_pool(usize bytes) {
    void *p = 0;
    if (BS->AllocatePool(EfiLoaderData, bytes, &p) != EFI_SUCCESS) return 0;
    return p;
}

static u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

/* ===================================================================
 * 4. 机械风格加载界面（真实进度，见 splash.c）
 * =================================================================== */
#include "splash.c"

/* ===================================================================
 * 5. 文件系统
 * =================================================================== */
static void efi_path_convert(const char *ascii, CHAR16 *out, usize max) {
    usize i = 0;
    if (i + 1 < max) out[i++] = '\\';
    const char *p = ascii;
    while (*p == '/' || *p == '\\') p++;
    while (*p && i + 1 < max) {
        out[i++] = (*p == '/') ? '\\' : (CHAR16)(u8)*p;
        p++;
    }
    out[i] = 0;
}

/* plan 阶段（g_plan=1）只取大小计入进度总量；need=1 的小清单文件仍真读，
 * 供调用方解析出后续要装载的文件列表。 */
static int fs_read(const char *path, void **out, u64 *out_size, int need) {
    CHAR16 wpath[300];
    EFI_FILE_PROTOCOL *f = 0;
    efi_path_convert(path, wpath, 300);
    EFI_STATUS st = RootFs->Open(RootFs, &f, wpath, EFI_FILE_MODE_READ, 0);
    if (st != EFI_SUCCESS || !f) { if (!g_plan) log_hex2("[boot]   open st=", st); return -1; }

    u8 info_buf[1024] __attribute__((aligned(8)));
    usize info_sz = sizeof(info_buf);
    st = f->GetInfo(f, (EFI_GUID *)&EFI_FILE_INFO_GUID, &info_sz, info_buf);
    if (st != EFI_SUCCESS) { log_hex2("[boot]   getinfo st=", st); f->Close(f); return -2; }
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info_buf;
    u64 size = fi->FileSize;
    if (fi->Attribute & EFI_FILE_DIRECTORY) { f->Close(f); return -3; }
    if (g_plan) {
        g_bytes_total += size;
        g_files_total++;
        if (!need) {
            f->Close(f);
            *out = 0;
            *out_size = size;
            return 0;
        }
    }

    u64 mem = alloc_pages(size ? size : 1);
    if (!mem) { log_hex2("[boot]   alloc failed size=", size); f->Close(f); return -4; }
    u8 *dst = (u8 *)mem;
    u64 got = 0;
    while (got < size) {
        usize chunk = (usize)((size - got) > (1u << 20) ? (1u << 20) : (size - got));
        st = f->Read(f, &chunk, dst + got);
        if (st != EFI_SUCCESS || chunk == 0) {
            log_hex2("[boot]   read st=", st);
            log_hex2("[boot]   read got=", got);
            f->Close(f); return -5;
        }
        got += chunk;
        if (!g_plan) g_bytes_loaded += chunk;
        anim_pump();
    }
    f->Close(f);
    if (!g_plan) g_files_done++;
    *out = dst;
    *out_size = size;
    return 0;
}

static int fs_read_file(const char *path, void **out, u64 *out_size) {
    return fs_read(path, out, out_size, 0);
}

/* ===================================================================
 * 6. Limine 兼容结构（与内核 arch/x86_64/limine.h 逐字段一致）
 * =================================================================== */
typedef struct {
    u64 revision;
    void *address;
    u64 size;
    char *path;
    char *cmdline;
    u32 media_type;
    u32 unused;
    u32 tftp_ip;
    u32 tftp_port;
    u32 partition_index;
    u32 mbr_disk_id;
    u8  gpt_disk_uuid[16];
    u8  gpt_part_uuid[16];
    u8  part_uuid[16];
} limine_file;

typedef struct {
    u64 revision;
    u64 module_count;
    limine_file **modules;
} limine_module_response;

typedef struct {
    u64 revision;
    u64 offset;
} limine_hhdm_response;

typedef struct {
    u64 revision;
    u64 physical_base;
    u64 virtual_base;
} limine_kaddr_response;

typedef struct {
    u64 revision;
    void *address;
} limine_rsdp_response;

typedef struct {
    void *address;
    u64 width;
    u64 height;
    u64 pitch;
    u16 bpp;
    u8 memory_model;
    u8 red_mask_size;
    u8 red_mask_shift;
    u8 green_mask_size;
    u8 green_mask_shift;
    u8 blue_mask_size;
    u8 blue_mask_shift;
    u8 unused[7];
    u64 edid_size;
    void *edid;
    u64 mode_count;
    void **modes;
} limine_framebuffer;

typedef struct {
    u64 revision;
    u64 framebuffer_count;
    limine_framebuffer **framebuffers;
} limine_fb_response;

typedef struct {
    u64 base;
    u64 length;
    u64 type;
} limine_memmap_entry;

typedef struct {
    u64 revision;
    u64 entry_count;
    limine_memmap_entry **entries;
} limine_memmap_response;

/* 内核 request 的 4 个 ID 字（magic + 请求号） */
static const u64 ID_MODULE[4]  = { 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                   0x3e7e279702be32afULL, 0xca1c4f3bd1280ceeULL };
static const u64 ID_RSDP[4]    = { 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                   0xc5e77b6b397e7b43ULL, 0x27637845accdcf3cULL };
static const u64 ID_FB[4]      = { 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                   0x9d5827dcd881dd75ULL, 0xa3148604f6fab11bULL };
static const u64 ID_HHDM[4]    = { 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                   0x48dcf1cb8ad2b852ULL, 0x63984e959a98244bULL };
static const u64 ID_KADDR[4]   = { 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                   0x71ba76863cc55f63ULL, 0xb2644a48c516a487ULL };
static const u64 ID_MEMMAP[4]  = { 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL,
                                   0x67cf3d9d378a806fULL, 0xe304acdfc50c3c62ULL };

#define HHDM_BASE 0xffff800000000000ULL
#define KERNEL_VBASE 0xffffffff80000000ULL

/* ===================================================================
 * 7. 模块表
 * =================================================================== */
#define MAX_MODULES 192
static limine_file g_mods[MAX_MODULES];
static char g_mod_paths[MAX_MODULES][192];
static char g_mod_cmd[MAX_MODULES][64];
static u32 g_mod_count;

static int add_module(const char *efi_path_ascii, void *data, u64 size, const char *cmdline) {
    if (g_mod_count >= MAX_MODULES) return -1;
    limine_file *m = &g_mods[g_mod_count];
    memset(m, 0, sizeof(*m));
    m->revision = 0;
    m->address = data;
    m->size = size;
    str_copy(g_mod_paths[g_mod_count], efi_path_ascii, sizeof(g_mod_paths[0]));
    str_copy(g_mod_cmd[g_mod_count], cmdline, sizeof(g_mod_cmd[0]));
    m->path = g_mod_paths[g_mod_count];
    m->cmdline = g_mod_cmd[g_mod_count];
    m->media_type = 0;
    g_mod_count++;
    return 0;
}

static int load_module_file(const char *path, const char *cmdline) {
    void *data = 0;
    u64 size = 0;
    int rc = fs_read_file(path, &data, &size);
    if (rc != 0) {
        if (!g_plan) log2("[boot] module read failed: ", path);
        return rc;
    }
    add_module(path, data, size, cmdline);
    if (!g_plan) log2("[boot] loaded ", path);
    return 0;
}

static int module_path_loaded(const char *path) {
    for (u32 i = 0; i < g_mod_count; i++) {
        if (str_eq(g_mods[i].path, path)) return 1;
    }
    return 0;
}

/* 按 manifest.json 中声明的 "path" 逐项装载。
 * 不递归扫描 driver/ 目录：SYSTEM/driver 里存在历史遗留的重复 .drv
 * （根目录副本 + 子目录正本），全量装载会重复注册总线服务 → BUS-E02。 */
static void load_driver_modules_from_manifest(const u8 *txt, u64 len) {
    for (u64 i = 0; i + 6 < len; i++) {
        if (!str_ieq_n((const char *)txt + i, "\"path\"", 6)) continue;
        u64 j = i + 6;
        while (j < len && (txt[j] == ' ' || txt[j] == '\t' || txt[j] == ':')) j++;
        if (j >= len || txt[j] != '"') continue;
        j++;
        char path[192];
        usize n = 0;
        while (j < len && txt[j] != '"' && n + 1 < sizeof(path)) path[n++] = (char)txt[j++];
        path[n] = 0;
        if (!n || path[0] == '/') continue;
        char full[200];
        full[0] = '/';
        str_copy(full + 1, path, sizeof(full) - 1);
        if (module_path_loaded(full)) continue;

        char base[128];
        str_copy(base, path, sizeof(base));
        /* 取 basename 并去掉扩展名作为 cmdline 后缀 */
        char *bn = base;
        for (char *p = base; *p; p++) if (*p == '/') bn = p + 1;
        usize bl = str_len(bn);
        if (bl > 4 && bn[bl - 4] == '.') bn[bl - 4] = 0;
        char cmd[64];
        str_copy(cmd, "dkm:", sizeof(cmd));
        str_cat(cmd, bn, sizeof(cmd));
        load_module_file(full, cmd);
    }
}

/* ===================================================================
 * 8. ELF64 装载（UTSM 为静态 ET_EXEC）
 * =================================================================== */
typedef struct {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf64_ehdr;

typedef struct {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} elf64_phdr;

static u64 g_kernel_phys;
static u64 g_kernel_span;
static u64 g_kernel_entry_phys;
static u64 g_kernel_entry_virt;

static int load_kernel_elf(const void *data) {
    const elf64_ehdr *eh = (const elf64_ehdr *)data;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' || eh->ident[3] != 'F') {
        log2("[boot] BOOT-E01 not ELF", 0);
        return -1;
    }
    if (eh->type != 2 || eh->machine != 62) {
        log2("[boot] BOOT-E02 not ET_EXEC/x86_64", 0);
        return -2;
    }
    u64 min_va = ~0ULL, max_end = 0;
    for (u16 i = 0; i < eh->phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)((const u8 *)data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != 1) continue;
        if (ph->vaddr < min_va) min_va = ph->vaddr;
        if (ph->vaddr + ph->memsz > max_end) max_end = ph->vaddr + ph->memsz;
    }
    if (min_va == ~0ULL || max_end <= min_va) return -3;

    u64 span = align_up(max_end - min_va, 1 << 21);
    u64 raw = alloc_pages(span + (1 << 21));
    if (!raw) { log2("[boot] BOOT-E03 kernel alloc failed", 0); return -4; }
    u64 phys = align_up(raw, 1 << 21);
    memset((void *)phys, 0, span);

    for (u16 i = 0; i < eh->phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)((const u8 *)data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != 1) continue;
        u8 *dst = (u8 *)(phys + (ph->vaddr - min_va));
        const u8 *src = (const u8 *)data + ph->offset;
        if (ph->filesz) memcpy(dst, src, ph->filesz);
        /* memsz > filesz 的部分已由 memset 清零 */
    }

    g_kernel_phys = phys;
    g_kernel_span = span;
    g_kernel_entry_virt = eh->entry;
    g_kernel_entry_phys = phys + (eh->entry - min_va);
    log_hex2("[boot] kernel phys=", phys);
    log_hex2("[boot] kernel span=", span);
    log_hex2("[boot] kernel entry=", eh->entry);

    /* 扫描 Limine request 在镜像中的偏移 */
    return 0;
}

/* 在已装载内核镜像中定位 request（返回镜像内偏移；0 = 未找到） */
static u64 find_request(u64 phys_img, u64 img_len, const u64 id[4]) {
    for (u64 off = 0; off + 48 <= img_len; off += 8) {
        const u64 *p = (const u64 *)(phys_img + off);
        if (p[0] == id[0] && p[1] == id[1] && p[2] == id[2] && p[3] == id[3]) return off;
    }
    return 0;
}

/* ===================================================================
 * 9. 页表
 * =================================================================== */
#define PTE_P 1ULL
#define PTE_W 2ULL
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PTE_PS (1ULL << 7)

static u64 pml4_new_page(void) {
    u64 p = alloc_pages(EFI_PAGE_SIZE);
    if (p) memset((void *)p, 0, EFI_PAGE_SIZE);
    return p;
}

/* 把 [start,end) 的恒等映射拆成 4KiB 页并标记 UC（PCD|PWT）。
 * 帧缓冲必须不可缓存：部分固件把 GOP 缓冲放在 PCIe BAR/VRAM 光圈里，
 * 用 WB 写会留在 CPU cache，扫描输出引擎读不到 → 花屏/停在旧画面。 */
static void map_range_uncached(u64 pml4, u64 start, u64 end) {
    if (end <= start) return;
    const u64 BLK = 1ULL << 21;
    start &= ~(BLK - 1);
    end = (end + BLK - 1) & ~(BLK - 1);
    for (u64 a = start; a < end; a += BLK) {
        u64 *p4 = (u64 *)pml4;
        u64 i4 = (a >> 39) & 0x1FF, i3 = (a >> 30) & 0x1FF, i2 = (a >> 21) & 0x1FF;
        if (!(p4[i4] & PTE_P)) continue;
        u64 *pdpt = (u64 *)(p4[i4] & ~0xFFFULL);
        if (!(pdpt[i3] & PTE_P)) continue;
        u64 *pd = (u64 *)(pdpt[i3] & ~0xFFFULL);
        if (!(pd[i2] & PTE_P)) continue;
        u64 pt_phys = pml4_new_page();
        if (!pt_phys) return;      /* 失败保留原 WB 映射，不中断启动 */
        u64 *pt = (u64 *)pt_phys;
        for (u64 k = 0; k < 512; k++) {
            pt[k] = (a + k * 4096ULL) | PTE_P | PTE_W | PTE_PCD | PTE_PWT;
        }
        pd[i2] = pt_phys | PTE_P | PTE_W;
    }
}

static u64 build_page_tables(u64 phys_max) {
    phys_max = align_up(phys_max, 1 << 21);
    if (phys_max < (1ULL << 32)) phys_max = (1ULL << 32);
    /* 上限 4TiB：实机 GOP 帧缓冲偶尔位于高位（PCIe BAR），必须在其上方
     * 仍能建立恒等/HHDM 映射；每 GB 一个 4KiB PD，4TiB 约 16MiB 页表。 */
    if (phys_max > (4ULL << 40)) phys_max = (4ULL << 40);

    u64 pml4 = pml4_new_page();
    if (!pml4) return 0;

    /* 恒等映射低物理地址（2MiB 大页）。
     * 注意：页表项 = 物理地址 | flags；解引用前必须先剥离 flags，
     * 否则会以 phys|3 为基址错位写坏页表（#GP）。 */
    for (u64 gb = 0; gb < phys_max; gb += (1ULL << 30)) {
        u64 pml4i = (gb >> 39) & 0x1FF;
        u64 *p4 = (u64 *)pml4;
        if (!(p4[pml4i] & PTE_P)) {
            u64 np = pml4_new_page();
            if (!np) return 0;
            p4[pml4i] = np | PTE_P | PTE_W;
        }
        u64 *pdpt = (u64 *)(p4[pml4i] & ~0xFFFULL);
        u64 pdpti = (gb >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_P)) {
            u64 np = pml4_new_page();
            if (!np) return 0;
            pdpt[pdpti] = np | PTE_P | PTE_W;
        }
        u64 *pd = (u64 *)(pdpt[pdpti] & ~0xFFFULL);
        for (u64 i = 0; i < 512; i++) {
            u64 pa = (gb & ~((1ULL << 30) - 1)) + i * (1ULL << 21);
            if (pa >= phys_max) break;
            pd[i] = pa | PTE_P | PTE_W | PTE_PS;
        }
    }

    /* HHDM：0xffff800000000000 起映射同一批物理页（共享 PDPT/PD） */
    {
        u64 *p4 = (u64 *)pml4;
        for (u64 i = 0; i < 256; i++) {
            if (p4[i]) p4[256 + i] = p4[i];
        }
    }

    /* 内核高半区 0xffffffff80000000（PML4[511], PDPT[510]） */
    {
        u64 *p4 = (u64 *)pml4;
        if (!(p4[511] & PTE_P)) {
            u64 np = pml4_new_page();
            if (!np) return 0;
            p4[511] = np | PTE_P | PTE_W;
        }
        u64 *pdpt = (u64 *)(p4[511] & ~0xFFFULL);
        u64 pd = pml4_new_page();
        if (!pd) return 0;
        pdpt[510] = pd | PTE_P | PTE_W;
        u64 *pt = (u64 *)pd;
        u64 page_count = g_kernel_span / (1ULL << 21);
        for (u64 i = 0; i < page_count && i < 512; i++) {
            pt[i] = (g_kernel_phys + i * (1ULL << 21)) | PTE_P | PTE_W | PTE_PS;
        }
    }

    /* 帧缓冲 UC 映射（恒等与 HHDM 共享同一批 PD，同时生效） */
    if (g_fb && g_fb_w && g_fb_h) {
        map_range_uncached(pml4, (u64)g_fb, (u64)g_fb + g_fb_h * g_fb_pitch);
        log2("[boot] framebuffer mapped uncached", 0);
    }

    return pml4;
}

/* ===================================================================
 * 10. ACPI / 内存映射 / Limine 响应
 * =================================================================== */
static void *find_rsdp(void) {
    for (usize i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &ST->ConfigurationTable[i];
        const u8 *g = (const u8 *)&ct->VendorGuid;
        const u8 *a = (const u8 *)&EFI_ACPI_20_TABLE_GUID;
        int same = 1;
        for (int k = 0; k < 16; k++) if (g[k] != a[k]) { same = 0; break; }
        if (same) return ct->VendorTable;
    }
    for (usize i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &ST->ConfigurationTable[i];
        const u8 *g = (const u8 *)&ct->VendorGuid;
        const u8 *a = (const u8 *)&EFI_ACPI_10_TABLE_GUID;
        int same = 1;
        for (int k = 0; k < 16; k++) if (g[k] != a[k]) { same = 0; break; }
        if (same) return ct->VendorTable;
    }
    return 0;
}

static u64 get_memory_map(u8 **out_map, usize *out_size, usize *out_desc_size,
                          usize *out_key, u32 *out_ver) {
    usize size = 0, key = 0, dsz = 0;
    u32 ver = 0;
    EFI_STATUS st = BS->GetMemoryMap(&size, 0, &key, &dsz, &ver);
    if (st != EFI_BUFFER_TOO_SMALL && st != EFI_SUCCESS) return st;
    size += 8 * dsz;
    u8 *buf = (u8 *)alloc_pool(size);
    if (!buf) return EFI_ERROR_BIT;
    st = BS->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)buf, &key, &dsz, &ver);
    if (st != EFI_SUCCESS) { BS->FreePool(buf); return st; }
    *out_map = buf;
    *out_size = size;
    *out_desc_size = dsz;
    *out_key = key;
    *out_ver = ver;
    return EFI_SUCCESS;
}

static u64 phys_max_from_map(u8 *map, usize size, usize dsz) {
    u64 mx = 0;
    for (usize off = 0; off + dsz <= size; off += dsz) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(map + off);
        /* MMIO 空洞（常见于 >1TB 的 PCI 窗口）不参与 RAM 映射；
         * 帧缓冲由调用方单独抬高上限。 */
        if (d->Type == EfiMemoryMappedIO || d->Type == EfiMemoryMappedIOPortSpace) continue;
        u64 end = d->PhysicalStart + d->NumberOfPages * EFI_PAGE_SIZE;
        if (end > mx) mx = end;
    }
    return mx;
}

static limine_memmap_response *build_memmap_response(u8 *map, usize size, usize dsz) {
    u64 count = 0;
    for (usize off = 0; off + dsz <= size; off += dsz) count++;
    limine_memmap_entry *ents = (limine_memmap_entry *)alloc_pool(count * sizeof(limine_memmap_entry));
    limine_memmap_entry **ptrs = (limine_memmap_entry **)alloc_pool(count * sizeof(void *));
    limine_memmap_response *rsp = (limine_memmap_response *)alloc_pool(sizeof(*rsp));
    if (!ents || !ptrs || !rsp) return 0;

    u64 n = 0;
    for (usize off = 0; off + dsz <= size; off += dsz) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(map + off);
        u64 type;
        switch (d->Type) {
            case EfiConventionalMemory: type = 0; break;              /* USABLE */
            case EfiACPIReclaimMemory:  type = 2; break;
            case EfiACPIMemoryNVS:      type = 3; break;
            case EfiUnusableMemory:     type = 4; break;
            case EfiLoaderCode:
            case EfiLoaderData:
            case EfiBootServicesCode:
            case EfiBootServicesData:   type = 5; break;              /* BOOTLOADER_RECLAIMABLE */
            default:                    type = 1; break;              /* RESERVED */
        }
        ents[n].base = d->PhysicalStart;
        ents[n].length = d->NumberOfPages * EFI_PAGE_SIZE;
        ents[n].type = type;
        ptrs[n] = &ents[n];
        n++;
    }
    /* 内核镜像 + 全部 boot module（驱动/资产/FUCK/固件）标为
     * KERNEL_AND_MODULES，避免被内核 DMA 分配器当 USABLE 回收。
     * 与区间有重叠的条目整条标记（保守：宁可少给可用内存）。 */
    {
        u64 kn = g_kernel_phys, ke = g_kernel_phys + g_kernel_span;
        for (u64 i = 0; i < n; i++) {
            u64 b = ents[i].base, e = ents[i].base + ents[i].length;
            if (b < ke && e > kn) ents[i].type = 6;
        }
        for (u32 m = 0; m < g_mod_count; m++) {
            u64 mb = (u64)g_mods[m].address;
            u64 me = mb + g_mods[m].size;
            if (!mb) continue;
            for (u64 i = 0; i < n; i++) {
                u64 b = ents[i].base, e = ents[i].base + ents[i].length;
                if (b < me && e > mb) ents[i].type = 6;
            }
        }
    }
    rsp->revision = 0;
    rsp->entry_count = n;
    rsp->entries = ptrs;
    return rsp;
}

/* ===================================================================
 * 11. 引导末段：GDT / CR3 / 栈切换 / 跳转
 * =================================================================== */
static u64 g_gdt[8];
static u64 g_stack_phys;

static void build_gdt(void) {
    g_gdt[0] = 0;
    g_gdt[1] = 0x00AF9A000000FFFFULL;  /* 0x08: 64-bit code, DPL0 */
    g_gdt[2] = 0x00CF92000000FFFFULL;  /* 0x10: flat data, DPL0 */
}

__attribute__((noreturn))
static void jump_to_kernel(u64 entry, u64 cr3, u64 stack_top) {
    struct __attribute__((packed)) { u16 limit; u64 base; } gdtr;
    gdtr.limit = (u16)(sizeof(g_gdt) - 1);
    gdtr.base = (u64)g_gdt;

    __asm__ volatile("cli");
    __asm__ volatile("lgdt %0" :: "m"(gdtr));
    /* 重新加载 CS 与数据段 */
    __asm__ volatile(
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        ::: "rax", "memory");
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    __asm__ volatile("mov %0, %%rsp\n\tjmp *%1" :: "r"(stack_top), "r"(entry) : "memory");
    for (;;) { __asm__ volatile("cli; hlt"); }
}

/* ===================================================================
 * 12. 主流程
 * =================================================================== */
static void halt_forever(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---------------- GOP 模式选择 ----------------
 * 实机固件默认模式常是面板原生分辨率（1920x1080/2560x1440/4K）。桌面
 * 双缓冲只有 8MiB，且布局按桌面尺寸设计，超预算会越界/出屏。因此主动
 * SetMode 到已知安全模式；FUCK [boot] gop_width/gop_height 可强制指定。 */
static u32 gop_find_mode(u32 w, u32 h) {
    if (!GOP || !GOP->Mode) return 0xFFFFFFFFu;
    for (u32 i = 0; i < GOP->Mode->MaxMode; i++) {
        usize sz = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = 0;
        if (GOP->QueryMode(GOP, i, &sz, &mi) != EFI_SUCCESS || !mi) continue;
        if (mi->PixelFormat == PixelBltOnly) continue;
        if ((u64)mi->HorizontalResolution * (u64)mi->VerticalResolution * 4ULL > FB_SPRITE_BUDGET) continue;
        if (w && mi->HorizontalResolution != w) continue;
        if (h && mi->VerticalResolution != h) continue;
        return i;
    }
    return 0xFFFFFFFFu;
}

static u32 gop_largest_fitting_mode(void) {
    if (!GOP || !GOP->Mode) return 0xFFFFFFFFu;
    u32 best = 0xFFFFFFFFu;
    u64 best_px = 0;
    for (u32 i = 0; i < GOP->Mode->MaxMode; i++) {
        usize sz = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = 0;
        if (GOP->QueryMode(GOP, i, &sz, &mi) != EFI_SUCCESS || !mi) continue;
        if (mi->PixelFormat == PixelBltOnly) continue;
        u64 px = (u64)mi->HorizontalResolution * (u64)mi->VerticalResolution;
        if (px * 4ULL > FB_SPRITE_BUDGET) continue;
        if (px > best_px) { best_px = px; best = i; }
    }
    return best;
}

static void gop_apply_info(void) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = GOP && GOP->Mode ? GOP->Mode->Info : 0;
    if (!mi) return;
    g_fb = (u8 *)(u64)GOP->Mode->FrameBufferBase;
    g_fb_w = mi->HorizontalResolution;
    g_fb_h = mi->VerticalResolution;
    /* 少数固件把 PixelsPerScanLine 报成 0 或小于横向分辨率；按宽度兜底，
     * 否则 pitch 过小会导致逐行横向错位（实机上表现为整屏画面偏移）。 */
    {
        u64 pps = mi->PixelsPerScanLine;
        if (pps < mi->HorizontalResolution) pps = mi->HorizontalResolution;
        g_fb_pitch = pps * 4;
    }
    g_fb_pixfmt = (u32)mi->PixelFormat;
    if (g_fb_pixfmt == 0) {          /* RedGreenBlueReserved */
        g_fb_rsh = 0; g_fb_gsh = 8; g_fb_bsh = 16;
        g_fb_rsz = g_fb_gsz = g_fb_bsz = 8;
    } else if (g_fb_pixfmt == 1) {   /* BlueGreenRedReserved */
        g_fb_rsh = 16; g_fb_gsh = 8; g_fb_bsh = 0;
        g_fb_rsz = g_fb_gsz = g_fb_bsz = 8;
    } else {                         /* PixelBitMask：按固件掩码取 shift/size */
        u32 rm = mi->PixelInformation.RedMask;
        u32 gm = mi->PixelInformation.GreenMask;
        u32 bm = mi->PixelInformation.BlueMask;
        g_fb_rsh = 0; while (rm && !(rm & 1)) { rm >>= 1; g_fb_rsh++; }
        g_fb_gsh = 0; while (gm && !(gm & 1)) { gm >>= 1; g_fb_gsh++; }
        g_fb_bsh = 0; while (bm && !(bm & 1)) { bm >>= 1; g_fb_bsh++; }
        g_fb_rsz = 0; while (rm) { if (rm & 1) g_fb_rsz++; rm >>= 1; }
        g_fb_gsz = 0; while (gm) { if (gm & 1) g_fb_gsz++; gm >>= 1; }
        g_fb_bsz = 0; while (bm) { if (bm & 1) g_fb_bsz++; bm >>= 1; }
    }
}

static void gop_init_and_select(void) {
    void *gop = 0;
    if (BS->LocateProtocol((EFI_GUID *)&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, 0, &gop) != EFI_SUCCESS || !gop) {
        log2("[boot] WARN: GOP unavailable", 0);
        return;
    }
    GOP = (EFI_GRAPHICS_OUTPUT_PROTOCOL *)gop;
    if (!GOP->Mode) return;
    gop_apply_info();
    log_hex2("[boot] gop current w=", g_fb_w);
    log_hex2("[boot] gop current h=", g_fb_h);
    /* 枚举可用模式（实机诊断：贴串口日志即可确认面板支持哪些模式） */
    log_hex2("[boot] gop max_mode=", (u64)GOP->Mode->MaxMode);
    for (u32 i = 0; i < GOP->Mode->MaxMode; i++) {
        usize sz = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = 0;
        if (GOP->QueryMode(GOP, i, &sz, &mi) != EFI_SUCCESS || !mi) continue;
        serial_str("[boot]   mode ");
        serial_hex(i);
        serial_str(" = ");
        serial_hex(mi->HorizontalResolution);
        serial_str("x");
        serial_hex(mi->VerticalResolution);
        serial_str(" fmt=");
        serial_hex((u64)mi->PixelFormat);
        serial_str("\n");
    }

    /* 当前模式已满足预算且未强制指定 → 保留 */
    int fits = (u64)g_fb_w * (u64)g_fb_h * 4ULL <= FB_SPRITE_BUDGET;
    u32 pick = 0xFFFFFFFFu;
    if (g_want_w && g_want_h) {
        pick = gop_find_mode(g_want_w, g_want_h);
        log_hex2("[boot] gop forced mode=", (u64)pick);
    }
    if (pick == 0xFFFFFFFFu) {
        static const u16 pref[][2] = {
            {1280, 800}, {1280, 720}, {1366, 768}, {1600, 900}, {1600, 1200}, {1024, 768}
        };
        for (u32 i = 0; i < sizeof(pref) / sizeof(pref[0]); i++) {
            pick = gop_find_mode(pref[i][0], pref[i][1]);
            if (pick != 0xFFFFFFFFu) break;
        }
    }
    if (pick == 0xFFFFFFFFu && !fits) pick = gop_largest_fitting_mode();
    if (pick != 0xFFFFFFFFu) {
        EFI_STATUS ms = GOP->SetMode(GOP, pick);
        log_hex2("[boot] gop setmode st=", ms);
        gop_apply_info();
    }
    log_hex2("[boot] fb phys=", (u64)g_fb);
    log_hex2("[boot] fb w=", g_fb_w);
    log_hex2("[boot] fb h=", g_fb_h);
    log_hex2("[boot] fb pitch=", g_fb_pitch);
    log_hex2("[boot] fb bytes=", (u64)g_fb_h * g_fb_pitch);
    log_hex2("[boot] fb pixfmt=", g_fb_pixfmt);
    log_hex2("[boot] fb rsh=", g_fb_rsh);
    log_hex2("[boot] fb gsh=", g_fb_gsh);
    log_hex2("[boot] fb bsh=", g_fb_bsh);
}

static u32 parse_dec(const char *s) {
    u32 v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (u32)(*s - '0'); s++; }
    return v;
}

/* 引导载荷装载。执行两遍：g_plan=1 时只统计大小（进度条总量），
 * 随后恢复模块表再真正读入。 */
static void load_payloads(void **kdata, u64 *ksize) {
    /* 启动卷标识伪模块：4 字节卷序列号 */
    if (g_vol_serial) {
        add_module("/boot/volid", &g_vol_serial, 4, "boot:volid");
    }

    /* --- manifest + 按清单装载驱动 --- */
    {
        void *mdata = 0; u64 msize = 0;
        if (fs_read("/driver/manifest.json", &mdata, &msize, 1) != 0) {
            log2("[boot] BOOT-E13 manifest missing", 0); splash_fail(); halt_forever();
        }
        add_module("/driver/manifest.json", mdata, msize, "dkm:manifest");
        if (!g_plan) { log2("[boot] loaded manifest", 0); splash_stage("LOADING DRIVERS"); }
        load_driver_modules_from_manifest((const u8 *)mdata, msize);
    }
    /* MediaTek 固件（不在 manifest 内，由 fw:* cmdline 定位；缺失时驱动自行报错） */
    load_module_file("/driver/fw/mediatek/MT7920PA.BIN", "fw:mt7920_patch");
    load_module_file("/driver/fw/mediatek/MT7920RAM.BIN", "fw:mt7920_ram");

    /* 桌面资源预载（asset:*）：实机上内置盘常抢占块设备 0，且 USB 存储
     * 驱动的大文件读取在部分控制器上不稳定 —— 字库/壁纸/贴图直接由
     * 引导器送入内存，desktop 优先消费这些 boot module。 */
    if (!g_plan) splash_stage("LOADING DESKTOP ASSETS");
    load_module_file("/system/font/simhei_16.dbf", "asset:font16");
    load_module_file("/system/font/simhei_24.dbf", "asset:font24");
    load_module_file("/system/wallpaper.rgba", "asset:wallpaper");
    load_module_file("/system/deshab64/desktop/textures/back.rgba", "asset:backrgba");
    load_module_file("/system/deshab64/desktop/textures/bootlogo.rgba", "asset:bootlogo");
    load_module_file("/system/deshab64/desktop/textures/startMenuLogo.rgba", "asset:smlogo");

    /* --- ZTS 热更模块（SYSTEM/zts → ESP 根 /zts） ---
     * ZTS.CNF 每行首字段是文件名。全部预加载为 boot module；zts.drv 优先
     * 从 boot module 取，运行期写入仍走 FAT32 服务。 */
    {
        void *zc = 0; u64 zcs = 0;
        if (fs_read("/zts/ZTS.CNF", &zc, &zcs, 1) == 0) {
            add_module("/zts/ZTS.CNF", zc, zcs, "zts:manifest");
            if (!g_plan) { log2("[boot] loaded ZTS.CNF", 0); splash_stage("LOADING ZTS MODULES"); }
            const char *t = (const char *)zc;
            for (u64 i = 0; i < zcs; ) {
                u64 e = i;
                while (e < zcs && t[e] != '\n') e++;
                u64 s = i;
                while (s < e && (t[s] == ' ' || t[s] == '\t' || t[s] == '\r')) s++;
                if (s < e && t[s] != '#' && t[s] != ';') {
                    char fn[64]; usize n = 0;
                    while (s < e && t[s] != '|' && t[s] != ' ' && t[s] != '\t' &&
                           t[s] != '\r' && n + 1 < sizeof(fn)) fn[n++] = (char)t[s++];
                    fn[n] = 0;
                    if (n && !str_ieq_n(fn, "file", 4)) {
                        char full[96];
                        full[0] = 0;
                        str_cat(full, "/zts/", sizeof(full));
                        str_cat(full, fn, sizeof(full));
                        char cmd[80];
                        cmd[0] = 0;
                        str_cat(cmd, "zts:module:", sizeof(cmd));
                        str_cat(cmd, fn, sizeof(cmd));
                        if (!module_path_loaded(full)) load_module_file(full, cmd);
                    }
                }
                i = (e < zcs) ? e + 1 : zcs;
            }
        } else {
            if (!g_plan) log2("[boot] WARN: /zts/ZTS.CNF missing (zts autoload disabled)", 0);
        }
    }

    /* --- ZT5 guest 子系统载荷（P3） ---
     * 内核 cmdline "zt5:kernel"；用户程序 cmdline "zt5:<guest 路径>"，
     * zt5_loader 会把它们转成 guest 的 Limine module 表。 */
    {
        void *zk = 0; u64 zks = 0;
        if (!g_plan) splash_stage("LOADING ZT5 GUEST");
        if (fs_read_file("/boot/zt5.krl", &zk, &zks) == 0) {
            add_module("/boot/zt5.krl", zk, zks, "zt5:kernel");
            if (!g_plan) log2("[boot] loaded zt5.krl", 0);
            static const struct { const char *host; const char *guest; } zfiles[] = {
                { "/boot/zt5/prgm/sys/init",          "/prgm/sys/init" },
                { "/boot/zt5/prgm/sys/desk",          "/prgm/sys/desk" },
                { "/boot/zt5/prgm/sys/zrdemage",      "/prgm/sys/zrdemage" },
                { "/boot/zt5/prgm/sys/zinstall",      "/prgm/sys/zinstall" },
                { "/boot/zt5/prgm/shell",             "/prgm/shell" },
                { "/boot/zt5/prgm/windowprogram/fm",  "/prgm/windowprogram/fm" },
                { "/boot/zt5/prgm/windowprogram/zinstall", "/prgm/windowprogram/zinstall" },
                { "/boot/zt5/system/zratro.conf",     "/system/zratro.conf" },
            };
            for (u32 i = 0; i < sizeof(zfiles) / sizeof(zfiles[0]); i++) {
                char cmd[80];
                cmd[0] = 0;
                str_cat(cmd, "zt5:", sizeof(cmd));
                str_cat(cmd, zfiles[i].guest, sizeof(cmd));
                load_module_file(zfiles[i].host, cmd);
            }
        } else {
            if (!g_plan) log2("[boot] ZT5 guest payload absent (zt5.krl)", 0);
        }
    }

    /* --- Linux guest 载荷（仅 FUCK 开启时；rootfs 体积大） --- */
    if (g_linux_guest) {
        if (!g_plan) splash_stage("LOADING LINUX GUEST");
        load_module_file("/boot/linux-bzImage", "linux:bzImage");
        load_module_file("/boot/linux-initrd.img", "linux:initrd");
        load_module_file("/boot/linux-rootfs.img", "linux:rootfs");
        if (g_vscode_install) load_module_file("/boot/linux-vscode.tar.gz", "linux:vscode");
    }

    /* --- Windows guest firmware and read-only memory ESP --- */
    if (g_windows_guest) {
        if (!g_plan) splash_stage("LOADING WINDOWS GUEST");
        load_module_file("/boot/windows-vm/OVMF_CODE_4M.fd", "windows:ovmf-code");
        load_module_file("/boot/windows-vm/OVMF_VARS_4M.fd", "windows:ovmf-vars");
        load_module_file("/boot/windows-vm/windows-esp.img", "windows:esp");
    }

    /* --- UTSM 内核 --- */
    if (!g_plan) splash_stage("LOADING UTSM KERNEL");
    if (fs_read_file("/boot/utsm.elf", kdata, ksize) != 0) {
        log2("[boot] BOOT-E14 utsm.elf missing", 0); splash_fail(); halt_forever();
    }
}

__attribute__((noreturn))
void efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    ImageHandle = image;
    ST = st;
    BS = st->BootServices;
    serial_init();
    log2("[boot] Deshab EFI bootloader start", 0);

    /* --- 根文件系统（先建立：FUCK 决定 GOP 模式与可选大模块） --- */
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    if (BS->HandleProtocol(image, (EFI_GUID *)&EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&li) != EFI_SUCCESS || !li) {
        log2("[boot] BOOT-E10 LoadedImage unavailable", 0); halt_forever();
    }
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = 0;
    if (BS->HandleProtocol(li->DeviceHandle, (EFI_GUID *)&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void **)&sfs) != EFI_SUCCESS || !sfs) {
        log2("[boot] BOOT-E11 SimpleFileSystem unavailable", 0); halt_forever();
    }
    if (sfs->OpenVolume(sfs, &RootFs) != EFI_SUCCESS || !RootFs) {
        log2("[boot] BOOT-E12 OpenVolume failed", 0); halt_forever();
    }

    /* 读取启动卷的 FAT32 卷序列号（BPB offset 67），作为 "boot:volid"
     * 伪模块传给内核。实机多盘时 fat32.drv 用它精确锁定启动卷，
     * 避免 AHCI/NVMe 的内置盘抢在 xHCI 启动 U 盘前面成为块设备 0。 */
    {
        EFI_BLOCK_IO_PROTOCOL *bio = 0;
        if (BS->HandleProtocol(li->DeviceHandle, (EFI_GUID *)&EFI_BLOCK_IO_PROTOCOL_GUID,
                               (void **)&bio) == EFI_SUCCESS && bio && bio->Media) {
            log_hex2("[boot] blockio logical_partition=", (u64)bio->Media->LogicalPartition);
            u8 sec[512];
            EFI_STATUS rs = bio->ReadBlocks(bio, bio->Media->MediaId, 0, 512, sec);
            if (rs == EFI_SUCCESS && sec[510] == 0x55 && sec[511] == 0xAA) {
                g_vol_serial = (u32)sec[67] | ((u32)sec[68] << 8) |
                               ((u32)sec[69] << 16) | ((u32)sec[70] << 24);
                log_hex2("[boot] vol_serial=", g_vol_serial);
            } else {
                log_hex2("[boot] BPB read st=", rs);
            }
        } else {
            log2("[boot] WARN: BlockIo unavailable (volid unknown)", 0);
        }
    }

    /* --- FUCK：先读，决定 GOP 模式与是否装载 Linux guest 相关大模块 --- */
    {
        void *fuck = 0; u64 fsz = 0;
        if (fs_read_file("/system/deshab64/FUCK", &fuck, &fsz) == 0) {
            const char *txt = (const char *)fuck;
            for (usize i = 0; txt[i]; i++) {
                if (str_ieq_n(txt + i, "linux_guest=1", 13)) g_linux_guest = 1;
                if (str_ieq_n(txt + i, "windows_guest=1", 15)) g_windows_guest = 1;
                if (str_ieq_n(txt + i, "vscode_install=1", 16)) g_vscode_install = 1;
                if (str_ieq_n(txt + i, "gop_width=", 10))  g_want_w = parse_dec(txt + i + 10);
                if (str_ieq_n(txt + i, "gop_height=", 11)) g_want_h = parse_dec(txt + i + 11);
            }
            add_module("/system/deshab64/FUCK", fuck, fsz, "fuck:config");
            log2("[boot] loaded FUCK", 0);
            log_hex2("[boot] want gop w=", g_want_w);
            log_hex2("[boot] want gop h=", g_want_h);
        } else {
            log2("[boot] WARN: FUCK missing (kernel defaults)", 0);
        }
    }

    /* --- GOP：选择安全模式并绘制加载条 --- */
    gop_init_and_select();
    if (g_fb) splash_begin();
    void *kdata = 0; u64 ksize = 0;

    {
        u32 base_mods = g_mod_count;
        g_plan = 1;
        splash_stage("SCANNING VOLUME");
        load_payloads(&kdata, &ksize);
        g_plan = 0;
        g_mod_count = base_mods;
        g_bytes_loaded = 0;
        g_files_done = 0;
        log_hex2("[boot] payload files=", g_files_total);
        log_hex2("[boot] payload bytes=", g_bytes_total);
        load_payloads(&kdata, &ksize);
        log_hex2("[boot] payload load ms=", spl_ms());
    }
    splash_phase(9000, "RELOCATING KERNEL");
    if (load_kernel_elf(kdata) != 0) { splash_fail(); halt_forever(); }
    splash_phase(9300, "BUILDING PAGE TABLES");

    /* --- 页表 --- */
    u8 *mmap_buf = 0; usize mmap_size = 0, mmap_dsz = 0, mmap_key = 0; u32 mmap_ver = 0;
    if (get_memory_map(&mmap_buf, &mmap_size, &mmap_dsz, &mmap_key, &mmap_ver) != EFI_SUCCESS) {
        log2("[boot] BOOT-E15 GetMemoryMap failed", 0); splash_fail(); halt_forever();
    }
    u64 pmax = phys_max_from_map(mmap_buf, mmap_size, mmap_dsz);
    if (g_fb) {
        u64 fb_end = (u64)g_fb + g_fb_pitch * g_fb_h;
        if (fb_end > pmax) pmax = fb_end;   /* 帧缓冲必须可写 */
    }
    log_hex2("[boot] phys_max=", pmax);
    u64 cr3 = build_page_tables(pmax);
    if (!cr3) { log2("[boot] BOOT-E16 page table alloc failed", 0); splash_fail(); halt_forever(); }
    log_hex2("[boot] page tables cr3=", cr3);
    splash_phase(9650, "PREPARING HANDOFF");

    g_stack_phys = alloc_pages(65536);
    if (!g_stack_phys) { log2("[boot] BOOT-E17 stack alloc failed", 0); splash_fail(); halt_forever(); }

    /* --- 构造 Limine 响应 --- */
    limine_module_response *mod_rsp = (limine_module_response *)alloc_pool(sizeof(*mod_rsp));
    limine_file **mod_ptrs = (limine_file **)alloc_pool(sizeof(void *) * (g_mod_count + 1));
    if (!mod_rsp || !mod_ptrs) { log2("[boot] BOOT-E18 module response alloc failed", 0); splash_fail(); halt_forever(); }
    for (u32 i = 0; i < g_mod_count; i++) mod_ptrs[i] = &g_mods[i];
    mod_rsp->revision = 0;
    mod_rsp->module_count = g_mod_count;
    mod_rsp->modules = mod_ptrs;

    limine_hhdm_response *hhdm_rsp = (limine_hhdm_response *)alloc_pool(sizeof(*hhdm_rsp));
    limine_kaddr_response *kaddr_rsp = (limine_kaddr_response *)alloc_pool(sizeof(*kaddr_rsp));
    limine_rsdp_response *rsdp_rsp = (limine_rsdp_response *)alloc_pool(sizeof(*rsdp_rsp));
    limine_fb_response *fb_rsp = (limine_fb_response *)alloc_pool(sizeof(*fb_rsp));
    limine_framebuffer *fb = (limine_framebuffer *)alloc_pool(sizeof(*fb));
    limine_framebuffer **fb_ptrs = (limine_framebuffer **)alloc_pool(sizeof(void *));
    if (!hhdm_rsp || !kaddr_rsp || !rsdp_rsp || !fb_rsp || !fb || !fb_ptrs) {
        log2("[boot] BOOT-E19 response alloc failed", 0); splash_fail(); halt_forever();
    }

    hhdm_rsp->revision = 0;
    hhdm_rsp->offset = HHDM_BASE;

    kaddr_rsp->revision = 0;
    kaddr_rsp->physical_base = g_kernel_phys;
    kaddr_rsp->virtual_base = KERNEL_VBASE;

    rsdp_rsp->revision = 0;
    rsdp_rsp->address = find_rsdp();
    log_hex2("[boot] rsdp=", (u64)rsdp_rsp->address);

    memset(fb, 0, sizeof(*fb));
    fb->address = g_fb;
    fb->width = g_fb_w;
    fb->height = g_fb_h;
    fb->pitch = g_fb_pitch;
    fb->bpp = 32;
    fb->memory_model = 1;     /* RGB */
    fb->red_mask_size = (u8)(g_fb_rsz ? g_fb_rsz : 8);
    fb->red_mask_shift = (u8)g_fb_rsh;
    fb->green_mask_size = (u8)(g_fb_gsz ? g_fb_gsz : 8);
    fb->green_mask_shift = (u8)g_fb_gsh;
    fb->blue_mask_size = (u8)(g_fb_bsz ? g_fb_bsz : 8);
    fb->blue_mask_shift = (u8)g_fb_bsh;
    if (!g_fb) { fb->width = 0; fb->height = 0; }
    fb_ptrs[0] = fb;
    fb_rsp->revision = 0;
    fb_rsp->framebuffer_count = 1;
    fb_rsp->framebuffers = fb_ptrs;

    limine_memmap_response *mm_rsp = build_memmap_response(mmap_buf, mmap_size, mmap_dsz);
    if (!mm_rsp) { log2("[boot] BOOT-E20 memmap response alloc failed", 0); splash_fail(); halt_forever(); }

    log_hex2("[boot] memmap entries=", mm_rsp->entry_count);

    /* --- 动画收尾 --- */
    splash_finish();
    log2("[boot] splash done", 0);

    /* --- ExitBootServices（GetMemoryMap 变更则重试） --- */
    {
        int ok = 0;
        for (int tries = 0; tries < 4 && !ok; tries++) {
            u8 *m2 = 0; usize s2 = 0, d2 = 0, k2 = 0; u32 v2 = 0;
            EFI_STATUS gm = get_memory_map(&m2, &s2, &d2, &k2, &v2);
            if (gm != EFI_SUCCESS) { log_hex2("[boot] GetMemoryMap retry failed, st=", gm); break; }
            log2("[boot] calling ExitBootServices", 0);
            EFI_STATUS es = BS->ExitBootServices(image, k2);
            log_hex2("[boot] ExitBootServices st=", es);
            if (es == EFI_SUCCESS) {
                ok = 1;
                /* 成功后 Boot Services 已失效，绝不能再调用任何 BS 函数 */
            } else if (m2) {
                BS->FreePool(m2);
            }
        }
        if (!ok) { log2("[boot] BOOT-E21 ExitBootServices failed", 0); splash_fail(); halt_forever(); }
    }
    log2("[boot] ExitBootServices done", 0);

    /* --- 回填 Limine request ---
     * 此时尚未切 CR3，内核高半区不可见；用恒等映射的物理地址写入。
     * 内核随后经自己的高半区虚拟地址读取，物理字节相同。 */
    {
        u8 *img = (u8 *)g_kernel_phys;
        u64 img_len = g_kernel_span;
        struct { const u64 *id; void *rsp; const char *name; } table[] = {
            { ID_MODULE, mod_rsp,   "module" },
            { ID_RSDP,   rsdp_rsp,  "rsdp" },
            { ID_FB,     fb_rsp,    "framebuffer" },
            { ID_HHDM,   hhdm_rsp,  "hhdm" },
            { ID_KADDR,  kaddr_rsp, "kaddr" },
            { ID_MEMMAP, mm_rsp,    "memmap" },
        };
        for (u32 i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
            u64 off = find_request(g_kernel_phys, img_len, table[i].id);
            if (!off) { log2("[boot] BOOT-E22 request not found: ", table[i].name); halt_forever(); }
            *(u64 *)(img + off + 32) = 0;                    /* revision */
            *(u64 *)(img + off + 40) = (u64)table[i].rsp;    /* response */
            log_hex2("[boot] request patched off=", off);
        }
    }

    build_gdt();
    log_hex2("[boot] jump cr3=", cr3);
    log_hex2("[boot] jump entry=", g_kernel_entry_phys);
    jump_to_kernel(g_kernel_entry_phys, cr3, g_stack_phys + 65536);
}
