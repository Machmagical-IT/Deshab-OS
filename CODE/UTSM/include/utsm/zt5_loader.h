#ifndef UTSM_ZT5_LOADER_H
#define UTSM_ZT5_LOADER_H

#include <utsm/types.h>

/* zt5_loader.h — ZT5 (ZratroOS) kernel as a UTSM VMX non-root guest subsystem.
 *
 * ZT5 内核为 fixed-link ET_EXEC（0xFFFFFFFF80000000），启动入口 `_start`，
 * 依赖 Limine 响应（HHDM/memmap/framebuffer/kernel_file/module/cmdline）。
 * 本加载器在 guest 物理内存中伪造这些响应并回填 ZT5 的 .limine_reqs 段。
 *
 * Guest GPA 布局:
 *   0x00000000-0x000FFFFF  保留 / 低 1MB（EPT identity）
 *   0x00020000-0x000FFFFF  boot area（页表/GDT/伪响应/模块表/文件内容）
 *   0x00100000-0x003FFFFF  framebuffer（1024x768x4）
 *   0x00400000-0x00FFFFFF  模块文件区（ZT5 用户程序 / .zts）
 *   0x01000000-0x04FFFFFF  内核镜像窗口（64MB；ZT5 .bss 约 56MB）
 *   0x06000000-0x45FFFFFF  guest RAM（默认 1GB，降级 512/256MB）
 */
#define ZT5_KERNEL_VADDR    0xFFFFFFFF80000000ULL
#define ZT5_HHDM_OFFSET     0xFFFF800000000000ULL

#define ZT5_BOOT_AREA_GPA   0x00020000ULL
#define ZT5_BOOT_AREA_SIZE  (120ULL * 4096)        /* 120 pages = 480KB */
#define ZT5_MODULES_GPA     0x00400000ULL
#define ZT5_MODULES_SIZE    (12ULL * 1024 * 1024)
#define ZT5_GUEST_KERNEL_GPA 0x01000000ULL
#define ZT5_GUEST_KERNEL_MAX (64ULL * 1024 * 1024)
#define ZT5_GUEST_FB_GPA    0x00100000ULL
#define ZT5_FB_WIDTH        1024
#define ZT5_FB_HEIGHT       768
#define ZT5_FB_BPP          4
#define ZT5_FB_SIZE         ((u64)ZT5_FB_WIDTH * ZT5_FB_HEIGHT * ZT5_FB_BPP)
#define ZT5_GUEST_RAM_GPA   0x06000000ULL
#define ZT5_GUEST_RAM_SIZE  0x40000000ULL          /* 1GB */
#define ZT5_GUEST_RAM_MIN   0x10000000ULL          /* 256MB */

/* boot area 内部布局（zt5_loader.c 使用同值）：GDT 0x0 / 页表 0x1000 /
 * 栈 0x9000(16KB) / 伪响应 0xD000。 */
#define ZT5_BOOT_STACK_TOP  (ZT5_BOOT_AREA_GPA + 0xD000ULL)

struct zt5_guest_info {
    u64 kernel_gpa;
    u64 kernel_entry;
    u64 kernel_size;
    u64 pgt_gpa;
    u64 gdt_gpa;
    u64 ram_gpa;
    u64 ram_size;
    u64 fb_gpa;
    u64 modules_gpa;
    u64 modules_size;
    u32 module_count;
    int loaded;
};

/* 在 Limine boot module 中查找 ZT5 内核（cmdline "zt5:kernel" 或路径含 zt5）。
 * 返回模块数据指针，*size_out 为大小；未找到返回 NULL。 */
void *zt5_find_kernel_module(u64 *size_out);

/* 解析 ELF、分配 guest 内存、构造页表/GDT/伪 Limine 响应/模块文件表、
 * 建立 EPT 映射。返回 0 成功。 */
int zt5_loader_init(void);

const struct zt5_guest_info *zt5_get_guest_info(void);

/* 配置 VMCS 并 vmlaunch ZT5 guest。guest 终止后返回 0；启动失败返回负值。 */
int zt5_launch(void);

/* 供 srv.zts.guest.status / kern.zt5.status 使用的文本状态。 */
int zt5_status_text(char *buf, u32 cap);

#endif /* UTSM_ZT5_LOADER_H */
