#ifndef UTSM_CONFIG_H
#define UTSM_CONFIG_H

#include <utsm/types.h>

/* ---- 编译期固定常量（不暴露给 FUCK 配置） ---- */
#define UTSM_CACHE_LINE_SIZE 64ULL
#define UTSM_PAGE_SIZE 4096ULL
#define UTSM_KERNEL_STACK_SIZE 16384ULL

/* ---- 编译期默认值宏（用于静态数组大小上限） ---- */
#define UTSM_MAX_SEGMENTS_DEFAULT 1024U
#define UTSM_MAX_CAPABILITIES_DEFAULT 256U
#define UTSM_MAX_PCKC_KEYS_DEFAULT 8U
/* P4 万物互联：arena 是编译期固定数组（arena.c），承载全部 DKM 驱动镜像 +
 * DSK 镜像。stage4 引入 desktop/firstinit/login/shell/cmd 等大模块后 64MB 不足
 * （arena out of memory → DSK 装载失败 DSL-E12），上调至 128MB。 */
#define UTSM_ARENA_SIZE_DEFAULT (128ULL * 1024ULL * 1024ULL)
#define UTSM_DIRTY_SHARD_PAGES_DEFAULT 1024ULL

/* ---- 运行期配置变量（由 kernel_main 从 FUCK 文件初始化） ---- */
extern u32 g_utsm_max_segments;
extern u32 g_utsm_max_capabilities;
extern u32 g_utsm_max_pckc_keys;
extern u64 g_utsm_arena_size;
extern u64 g_utsm_dirty_shard_pages;

/* ---- 保留旧宏名（兼容性），映射到默认值 ---- */
#define UTSM_MAX_SEGMENTS    UTSM_MAX_SEGMENTS_DEFAULT
#define UTSM_MAX_CAPABILITIES UTSM_MAX_CAPABILITIES_DEFAULT
#define UTSM_MAX_PCKC_KEYS   UTSM_MAX_PCKC_KEYS_DEFAULT
#define UTSM_ARENA_SIZE      UTSM_ARENA_SIZE_DEFAULT
#define UTSM_DIRTY_SHARD_PAGES UTSM_DIRTY_SHARD_PAGES_DEFAULT

#endif
