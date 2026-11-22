/* shell_abi.h — Shell 契约本地镜像（P4b 万物互联）
 *
 * 目的：让 shell.drv（stage4 DKM 模块）只依赖 dkm_shared.h（DKM ABI）+ 本头，
 * 与 UTSM 内核头（utsm/dsk.h、utsm/pe.h、utsm/linux_compat.h）彻底解耦。
 * 本头按 UTSM 头逐字段镜像公共契约，布局即为 ABI；UTSM 头改动必须同步此处。
 *
 * 镜像来源：
 *   - probe_info / PROBE_INFO_MAGIC   ← utsm/dsk.h（$probe 命令消费）
 *   - linux_compat_service / LINUX_COMPAT_MAGIC / linux_compat_scanout_info
 *                                       ← utsm/linux_compat.h（dsl 命令消费，只读契约）
 *   - DSK_BOOT_MAGIC / sh_boot_context ← utsm/dsk.h 的 dsk_boot_context 全量布局
 *
 * sh_boot_context 是「过渡兼容」：shell 的 run/path 命令仍要加载 FAT32 根目录/
 * bin 下由 DSK 调度体系编译出来的 PIE ELF 工具（bin 目录内 .elf），这些工具入口
 * 是 dsk_entry(const dsk_boot_context *)。故 driver_init 用 kernel_api 的 framebuffer/
 * dkm_kernel_api + 服务总线上取到的 pe/lxc/probe 指针，在本地构造一个布局与
 * dsk_boot_context 完全一致的镜像，交给子工具。P5（bin 目录 .elf 归位为 DKM）后删除。
 */
#ifndef DKM_SHELL_ABI_H
#define DKM_SHELL_ABI_H

#include "../dkm_shared.h"   /* u8/u16/u32/u64/i32 等基础类型 + DKM ABI */

/* ===================================================================
 *  probe_info —— 镜像 utsm/dsk.h（逐字段一致）
 *  经 dsk_boot_context.reserved[6] 由 DSK 注入；P4b 起改由服务总线
 *  kern.probe.info（getter 返回 const probe_info*）提供。
 * =================================================================== */
#define PROBE_INFO_MAGIC 0x50524F42u  /* "PRFB" — Probe inFo Buffer */

typedef struct probe_info {
    u32 magic;              /* PROBE_INFO_MAGIC */
    u32 version;            /* ABI 版本，当前 1 */
    u64 probe_buf_addr;     /* probe_entry 缓冲基地址（物理/HHDM 地址） */
    u32 probe_cap;          /* 缓冲容量（条目数） */
    u32 probe_head;         /* 当前写入位置（递增取模） */
    u32 probe_count;        /* 已写入总条目数 */
    u32 probe_dropped;      /* 因缓冲满而覆盖的条目数 */
    u32 instr_enabled;      /* g_instr_enabled 当前值 */
    u32 instr_probe_enable; /* g_instr_probe_enable 当前值 */
    u64 reserved[4];
} probe_info;

/* ===================================================================
 *  linux_compat_service —— 镜像 utsm/linux_compat.h（只读契约，字段照抄）
 *  经 dsk_boot_context.reserved[5] 由 DSK 注入；P4b 起改由服务总线
 *  kern.lxc.service（getter 返回 const linux_compat_service*）提供。
 * =================================================================== */
#define LINUX_COMPAT_MAGIC 0x4C4E58434F4D5000ULL  /* "LNXCOMP\0" */

/* virtio-gpu scanout 查询结果（服务表 gpu_get_scanout_info 的入参类型） */
struct linux_compat_scanout_info {
    void *host_vaddr;
    u32 width;
    u32 height;
    u32 stride;
    u32 dirty;
    u32 enabled;
    u32 dirty_x;
    u32 dirty_y;
    u32 dirty_w;
    u32 dirty_h;
    u32 cursor_visible;
    u32 cursor_x;
    u32 cursor_y;
    u32 cursor_hot_x;
    u32 cursor_hot_y;
    u32 cursor_w;
    u32 cursor_h;
    void *cursor_bitmap;
};

typedef struct linux_compat_service {
    u64 magic;        /* LINUX_COMPAT_MAGIC */

    int (*is_available)(void);
    int (*exec)(const char *path, int argc, const char *const *argv,
                char *stdout_buf, u64 stdout_cap, u64 *stdout_len,
                u64 *exit_code);
    int (*file_list)(const char *path, char *buf, u64 cap, u64 *out_len);
    int (*file_read)(const char *path, u64 offset, char *buf, u64 cap,
                     u64 *out_len, u64 *out_total);
    int (*status)(char *buf, u64 cap);
    int (*file_write)(const char *path, u64 offset, const char *buf, u64 len,
                      u64 *out_written);
    int (*file_pull)(const char *linux_path, const char *fat32_name,
                     u64 *out_size);
    int (*file_push)(const char *fat32_name, const char *linux_path,
                     u64 *out_size);
    void (*input_forward_keyboard)(u16 code, u32 value);
    void (*input_forward_mouse)(i32 dx, i32 dy, u8 buttons);
    int (*gpu_get_scanout_info)(struct linux_compat_scanout_info *out);
    int (*exec_async)(const char *path, int argc, const char *const *argv);
    int (*run_slice)(void);
} linux_compat_service;

/* ===================================================================
 *  DSK_BOOT_MAGIC + sh_boot_context —— 镜像 utsm/dsk.h 的 dsk_boot_context
 *  （全量布局，逐字段一致）。仅用于 run/path 命令把上下文转交给子 PIE ELF 工具。
 * =================================================================== */
#define DSK_BOOT_MAGIC 0x44534B31424F4F54ULL /* "DSK1BOOT" */

typedef struct sh_boot_context {
    u64 magic;
    u32 abi_version;
    u32 size;

    u64 flags;

    u64 hhdm_offset;
    u64 rsdp_address;

    u64 framebuffer_address;
    u64 framebuffer_width;
    u64 framebuffer_height;
    u64 framebuffer_pitch;
    u32 framebuffer_bpp;
    u32 reserved_fb;

    u64 boot_modules_response;

    u64 dkm_kernel_api;
    u64 dkm_driver_table;
    u64 dkm_driver_count;

    u64 utsm_state;
    u64 drr_state;

    u64 memory_map;
    u64 memory_map_count;
    u64 memory_map_entry_size;

    u64 kernel_stack_top;
    u64 reserved[8];
} sh_boot_context;

/* 子工具入口签名（与 utsm/dsk.h 的 dsk_entry_fn 一致）。 */
typedef void (*sh_dsk_entry_fn)(const sh_boot_context *context);

#endif /* DKM_SHELL_ABI_H */
