/* zts.h — ZTS7 动态算法 / 内核热更新子系统（Deshab DKM 实现）
 *
 * 源码母本: CODE/ZT5/SYSTEM/lib/Ztknl/hotupdate.c
 *           CODE/ZT5/SYSTEM/lib/Ztknl/zts_algorithm.c
 * 移植要点:
 *   - ZT5 原实现的 TODO（模块加载/真实检查点/差分插入删除/事务文件与模块 op/
 *     签名校验/目录加载）在本实现中全部落地，不再保留静默占位。
 *   - 模块格式: 760 字节 zts_header + 16 字节对齐 + ELF64 ET_REL 正文。
 *   - 重定位集合与 DKM 驱动加载器一致: R_X86_64_64/32/32S/PC32/PLT32。
 *   - 模块是外部输入: 校验/加载/init 失败一律回滚 + ZTS-E## 日志，不 panic 内核；
 *     内部不变量破坏（槽位越界、状态表损坏）走 ZTS-E90 panic。
 */
#ifndef DKM_ZTS_H
#define DKM_ZTS_H

#include "../dkm_shared.h"

/* ============================================================
 * 错误码（登记 docs/STATUS.md）
 * ============================================================ */
#define ZTS_E01_HEADER      "ZTS-E01 ZTS HEADER INVALID"
#define ZTS_E02_ELF         "ZTS-E02 ZTS ELF INVALID"
#define ZTS_E03_RELOC       "ZTS-E03 RELOCATION UNSUPPORTED"
#define ZTS_E04_SYMBOL      "ZTS-E04 SYMBOL UNRESOLVED"
#define ZTS_E05_CHECKSUM    "ZTS-E05 INTEGRITY CHECK FAILED"
#define ZTS_E06_DEP         "ZTS-E06 MODULE DEPENDENCY MISSING"
#define ZTS_E07_SLOT        "ZTS-E07 MODULE SLOT EXHAUSTED"
#define ZTS_E08_CKPT        "ZTS-E08 CHECKPOINT NOT FOUND"
#define ZTS_E09_DIFF        "ZTS-E09 DIFF PACKAGE INVALID"
#define ZTS_E10_STORE       "ZTS-E10 STATE STORE WRITE FAILED"
#define ZTS_E11_SIGNATURE   "ZTS-E11 SIGNATURE INVALID"
#define ZTS_E12_EXEC        "ZTS-E12 MODULE EXECUTE FAILED"
#define ZTS_E13_STATE       "ZTS-E13 STATE SLOT INVALID"
#define ZTS_E14_ARGS        "ZTS-E14 SERVICE ARGUMENTS INVALID"
#define ZTS_E90_INTERNAL    "ZTS-E90 INTERNAL INVARIANT BROKEN"

/* ============================================================
 * ZTS 模块格式（与 ZT5 include/zts_algorithm.h 布局一致）
 * ============================================================ */
#define ZTS_MAGIC          0x53545A00u
#define ZTS_FORMAT_VERSION 2u      /* v2 = ET_REL 正文（v1 为 ZT5 旧 ET_DYN 尝试） */
#define ZTS_API_VERSION    2u
#define ZTS_UUID_LEN       16
#define ZTS_MAX_NAME       64
#define ZTS_MAX_DESC       256
#define ZTS_MAX_AUTHOR     64
#define ZTS_MAX_LICENSE    32
#define ZTS_MAX_DEPS       16

/* 模块分类 */
#define ZTS_CAT_FILESYSTEM 0
#define ZTS_CAT_NETWORK    1
#define ZTS_CAT_CRYPTO     2
#define ZTS_CAT_COMPRESSION 3
#define ZTS_CAT_SCHEDULER  4
#define ZTS_CAT_MEMORY     5
#define ZTS_CAT_DRIVER     6
#define ZTS_CAT_PATCH      7
#define ZTS_CAT_INJECT     8
#define ZTS_CAT_CUSTOM     9

typedef struct __attribute__((packed)) {
    u32 magic;
    u16 version;
    u16 api_min_version;
    char name[ZTS_MAX_NAME];
    char description[ZTS_MAX_DESC];
    char author[ZTS_MAX_AUTHOR];
    char license[ZTS_MAX_LICENSE];
    u8 uuid[ZTS_UUID_LEN];
    u32 category;
    u32 major_version;
    u32 minor_version;
    u32 patch_version;
    u32 num_dependencies;
    u64 init_func_offset;      /* 相对 ELF 正文起点的文件偏移（0 = 按符号名查找） */
    u64 cleanup_func_offset;
    u64 execute_func_offset;
    u64 data_offset;
    u64 data_size;
    u32 checksum;              /* FNV-1a 32，覆盖 ELF 正文 */
    u8 signature[256];
} zts_header;

#define ZTS_HEADER_SIZE  ((u32)sizeof(zts_header))
#define ZTS_BODY_OFFSET  768u      /* align16(760) */

_Static_assert(sizeof(zts_header) == 760, "zts_header layout mismatch");

/* ============================================================
 * ZTS API 表（模块侧通过 extern zts_api 指针访问）
 * 前 13 项与 ZT5 zts_api_table 前缀兼容，后续为 Deshab 扩展。
 * ============================================================ */
typedef struct zts_api_table {
    u32 api_version;

    /* 内存 */
    void *(*alloc)(u64 size);
    void (*free)(void *ptr);
    void *(*realloc)(void *ptr, u64 size);

    /* 日志（level: "info"/"warn"/"error"） */
    void (*log_print)(const char *level, const char *msg);

    /* 时间 */
    u64 (*time_now)(void);

    /* VFS（经 srv.fs.fat32.read_path；write 为 0 表示只读后端） */
    int (*vfs_open)(const char *path, u32 mode);
    int (*vfs_close)(int fd);
    int (*vfs_read)(int fd, void *buf, u64 count);
    int (*vfs_write)(int fd, const void *buf, u64 count);

    /* 网络（当前 Deshab 无原始发送服务，保持 NULL；模块须显式探测） */
    int (*net_send)(const void *data, u64 len);
    int (*net_recv)(void *data, u64 len);

    /* 字符串/内存 */
    int (*strcmp)(const char *s1, const char *s2);
    void *(*memcpy)(void *dst, const void *src, u64 n);
    void *(*memset)(void *s, int c, u64 n);

    /* ---- Deshab 扩展（api_version >= 2） ---- */
    u32 ext_version;
    int (*bus_probe)(const char *name, void **out_fn);
    int (*fs_read_file)(const char *path, u8 **out, u32 *size);
    u64 (*time_ms)(void);
    u32 (*crc32)(const void *data, u64 size);
} zts_api_table;

/* ============================================================
 * 模块状态 / 描述
 * ============================================================ */
#define ZTS_STATE_UNLOADED 0
#define ZTS_STATE_LOADED   1
#define ZTS_STATE_ACTIVE   2
#define ZTS_STATE_UPDATING 3
#define ZTS_STATE_ROLLBACK 4

#define ZTS_MAX_MODULES  32
#define ZTS_SLOT_SIZE    (128u * 1024u)
#define ZTS_SLOT_COUNT   ZTS_MAX_MODULES
#define ZTS_MAX_CKPTS    8
#define ZTS_MAX_PATCHES  32
#define ZTS_MAX_VERSIONS 8
#define ZTS_MAX_MOD_NAME 64
#define ZTS_MAX_FILE     96

typedef struct {
    char name[ZTS_MAX_NAME];
    u8 uuid[ZTS_UUID_LEN];
    u32 category;
    u32 major, minor, patch;
    u32 priority;
    u32 state;
    int loaded;
    int initialized;

    u32 slot;                 /* 0xFFFFFFFF = 无 */
    u32 image_size;           /* 装载后的 span */
    u32 image_crc;            /* 装载镜像 CRC32 */
    void *code_base;          /* slot 基址（RWX，可执行） */
    char file[ZTS_MAX_FILE];  /* 源文件名（相对 ZTS 目录） */
    char source;              /* 'A' / 'B' = 持久化副本 */

    int (*init)(zts_api_table *api);
    int (*cleanup)(void);
    int (*execute)(void *input, void *output, void *ctx);
} zts_module;

/* ============================================================
 * 检查点（槽位不可变 + 注册表快照，恢复=重新指向旧槽）
 * ============================================================ */
typedef struct {
    u64 id;
    u64 timestamp_ms;
    char description[64];
    u32 module_count;
    zts_module modules[ZTS_MAX_MODULES];
    u32 patch_count;
    /* patch 快照：target + replacement + 原始字节 */
    u64 patch_target[ZTS_MAX_PATCHES];
    u64 patch_replacement[ZTS_MAX_PATCHES];
    u32 patch_orig_len[ZTS_MAX_PATCHES];
    u8  patch_orig[ZTS_MAX_PATCHES][32];
    u32 version_before;
} zts_checkpoint;

/* ============================================================
 * ftrace 式函数替换
 * ============================================================ */
typedef struct {
    u64 target_addr;
    u64 replacement_addr;
    u32 version;
    int active;
    char target_name[ZTS_MAX_MOD_NAME];
    char replacement_name[ZTS_MAX_MOD_NAME];
} zts_patch;

/* ============================================================
 * 差分更新
 * ============================================================ */
#define ZTS_DIFF_MAGIC      0x46464944u  /* 'DIFF' */
#define ZTS_DIFF_BLOCK_SIZE 4096u
#define ZTS_DIFF_MODIFY     1u
#define ZTS_DIFF_INSERT     2u
#define ZTS_DIFF_DELETE     3u

typedef struct __attribute__((packed)) {
    u32 magic;
    u32 version;
    u64 old_checksum;
    u64 new_checksum;
    u32 block_count;
    u32 flags;
} zts_diff_header;

typedef struct __attribute__((packed)) {
    u64 offset;
    u32 size;
    u32 type;
    /* 数据紧跟头部：modify/insert 携带 size 字节；delete 不带数据 */
} zts_diff_block;

/* 更新包（.zup）：全量或差分 */
#define ZTS_UPD_MAGIC  0x3150555Au  /* 'ZUP1' */
typedef struct __attribute__((packed)) {
    u32 magic;
    u32 version;
    u32 mode;          /* 0=全量 1=差分 */
    u32 payload_size;
    char name[ZTS_MAX_NAME];
    u8 uuid[ZTS_UUID_LEN];
    u32 old_crc;
    u32 new_crc;
} zts_update_header;

/* ============================================================
 * 事务
 * ============================================================ */
#define ZTS_TX_MAX 8
#define ZTS_TX_OP_MEM    0
#define ZTS_TX_OP_FILE   1
#define ZTS_TX_OP_MODULE 2

typedef struct zts_tx_op {
    int type;
    void *target;
    u8 *old_value;
    const u8 *new_value;
    u32 size;
    char file[ZTS_MAX_FILE];
    char arg[ZTS_MAX_MOD_NAME];
    struct zts_tx_op *next;
} zts_tx_op;

typedef struct {
    u64 id;
    int state;               /* 0 idle 1 preparing 2 prepared 3 committing 4 committed 5 aborting 6 aborted */
    zts_tx_op *ops;
    zts_tx_op *ops_tail;
    u32 op_count;
    u64 checkpoint_id;
} zts_transaction;

/* ============================================================
 * 更新状态机 / 统计
 * ============================================================ */
enum zts_update_state {
    ZTS_UPDATE_IDLE = 0,
    ZTS_UPDATE_DOWNLOADING,
    ZTS_UPDATE_VERIFYING,
    ZTS_UPDATE_PREPARING,
    ZTS_UPDATE_APPLYING,
    ZTS_UPDATE_COMPLETED,
    ZTS_UPDATE_FAILED,
    ZTS_UPDATE_ROLLING_BACK
};

typedef struct {
    u64 total_updates;
    u64 successful_updates;
    u64 failed_updates;
    u64 rollbacks;
    u64 total_bytes_updated;
} zts_stats;

/* ============================================================
 * 跨文件接口
 * ============================================================ */
/* zts_api.c */
void zts_core_init(const struct dkm_kernel_api *api, const struct dkm_log_api *log);
const struct dkm_kernel_api *zts_kapi(void);
const struct dkm_log_api *zts_log_api(void);
void zts_log(const char *msg);
void zts_log_hex(const char *prefix, u64 v);
void zts_log_dec(const char *prefix, u64 v);
u64 zts_now_ms(void);
void *zts_memcpy(void *dst, const void *src, u64 n);
void *zts_memset(void *dst, int c, u64 n);
int zts_strcmp(const char *a, const char *b);
u64 zts_strlen(const char *s);
int zts_str_eq(const char *a, const char *b);
void zts_str_copy(char *dst, const char *src, u32 cap);
u32 zts_crc32(const void *data, u64 size);
u64 zts_fnv1a64(const void *data, u64 size);
u32 zts_fnv1a32(const void *data, u64 size);
void *zts_alloc(u64 size);
void zts_free(void *ptr);
void *zts_realloc(void *ptr, u64 size);
u32 zts_lock(void);
void zts_unlock(void);
zts_api_table *zts_api_get(void);
int zts_symbol_register(const char *name, u64 addr);
u64 zts_symbol_lookup(const char *name);
void zts_symbols_init(void);

/* zts_module.c（slot 管理） */
void *zts_slot_base(u32 slot);
int zts_slot_alloc(void);
void zts_slot_ref(u32 slot);
void zts_slot_unref(u32 slot);

/* zts_elf.c */
int zts_elf_load(const u8 *body, u32 body_size, u32 slot, zts_module *out,
                 u64 *init_off, u64 *exec_off, u64 *cleanup_off);

/* zts_module.c */
void zts_modules_init(void);
zts_module *zts_module_find(const char *name);
zts_module *zts_module_find_uuid(const u8 uuid[ZTS_UUID_LEN]);
int zts_module_load_buffer(const u8 *data, u32 size, const char *file,
                           int autoload, zts_module **out);
int zts_module_load_file(const char *file, int autoload);
int zts_module_unload(const char *name);
int zts_module_execute(const char *name, void *in, void *out);
int zts_module_count(void);
zts_module *zts_module_at(u32 index);
int zts_module_hotupdate(const char *name, const char *file);
int zts_module_check_dependencies(const zts_header *hdr);
void zts_modules_shutdown(void);
void zts_set_pending_deps(const char *csv);
void zts_modules_restore(const zts_module *mods, u32 count);

/* zts_hotupdate.c */
void zts_hu_init(void);
u64 zts_ckpt_create(const char *desc);
int zts_ckpt_restore(u64 id);
int zts_ckpt_delete(u64 id);
int zts_ckpt_list(char *buf, u32 cap);
int zts_ckpt_latest_for_module(const char *name, u64 *out_id);
int zts_diff_create(const u8 *old_data, u32 old_size, const u8 *new_data, u32 new_size,
                    u8 *out, u32 out_cap, u32 *out_size);
int zts_diff_apply(const u8 *diff, u32 diff_size, u8 *data, u32 *size, u32 cap);
int zts_update_apply_package(const char *path);
int zts_patch_install(u64 target, u64 replacement, const char *target_name,
                      const char *replacement_name);
int zts_patch_uninstall(u64 target);
int zts_patch_list(char *buf, u32 cap);
u32 zts_version_snapshot(const char *desc);
int zts_version_rollback(u32 version);
u32 zts_version_current(void);
void zts_stats_get(zts_stats *out);
int zts_stats_text(char *buf, u32 cap);
int zts_state_text(char *buf, u32 cap);
int zts_update_get_state(void);
u64 zts_tx_begin(void);
int zts_tx_add_op(u64 id, int type, void *target, const u8 *old_value,
                  const u8 *new_value, u32 size, const char *file, const char *arg);
int zts_tx_commit(u64 id);
int zts_tx_rollback(u64 id);

/* zts_store.c */
typedef struct {
    char name[ZTS_MAX_NAME];
    u8 uuid[ZTS_UUID_LEN];
    u32 version;
    char file[32];
    char source;
} zts_state_mod;

void zts_store_init(void);
int zts_store_read(const char *file, u8 **out, u32 *size);      /* boot module 优先 */
int zts_store_write(const char *name, const u8 *data, u32 size); /* ZTS 目录内，接受 8.3/LFN */
int zts_store_write_state(void);
int zts_store_load_state(void);
const char *zts_store_manifest(void);
const zts_state_mod *zts_store_state_find(const char *name);
const char *zts_store_state_file(const char *name);

/* zts_main.c 对外总线服务（签名见 docs/RE/ZTS子系统设计.md） */
int zts_svc_list(char *buf, u32 cap);
int zts_svc_load(const char *file);
int zts_svc_unload(const char *name);
int zts_svc_execute(const char *args);   /* "name|in_hex|out_cap" 简化文本协议 */
int zts_svc_hotupdate(const char *args); /* "name|file" */
int zts_svc_rollback(const char *args);  /* "name" 或 "ckpt:<id>" */
int zts_svc_ckpt_create(const char *desc);
int zts_svc_ckpt_restore(u64 id);
int zts_svc_diff_apply(const char *path);
int zts_svc_patch_install(const char *args); /* "target|replacement|tname|rname" */
int zts_svc_patch_uninstall(const char *args);
int zts_svc_state(char *buf, u32 cap);
int zts_svc_stats(char *buf, u32 cap);
int zts_svc_guest_status(char *buf, u32 cap);

/* zts_selftest.c */
void zts_selftest(void);

#endif /* DKM_ZTS_H */
