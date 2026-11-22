#ifndef HOTUPDATE_H
#define HOTUPDATE_H

#include <stdint.h>
#include <stddef.h>

/* 内核模块类型 */
typedef enum {
    MODULE_CORE = 0,        /* 核心内核 */
    MODULE_DRIVER,          /* 驱动模块 */
    MODULE_SYSCALL,         /* 系统调用模块 */
    MODULE_FS,              /* 文件系统模块 */
    MODULE_ABI,             /* ABI 兼容层 */
    MODULE_SUBKERNEL,       /* 子内核 */
} module_type_t;

/* 模块状态 */
typedef enum {
    MODULE_STATE_UNLOADED = 0,
    MODULE_STATE_LOADED,
    MODULE_STATE_ACTIVE,
    MODULE_STATE_UPDATING,
    MODULE_STATE_ROLLBACK,
} module_state_t;

/* 模块信息 */
struct module_info {
    char name[32];
    char version[16];
    module_type_t type;
    module_state_t state;
    
    uint64_t base;
    uint64_t size;
    
    void *sym_table;        /* 符号表 */
    void *reloc_table;      /* 重定位表 */
    
    int (*on_load)(void);
    void (*on_unload)(void);
    int (*on_update)(void *new_module);
    
    struct module_info *next;
};

/* 更新检查点 */
struct update_checkpoint {
    uint64_t id;
    uint64_t timestamp;
    char description[64];
    
    /* 模块快照 */
    void *module_snapshot;
    size_t snapshot_size;
    
    /* 状态备份 */
    void *state_backup;
    size_t state_size;
};

/* 初始化热更新系统 */
void hotupdate_init(void);

/* 注册模块 */
int module_register(struct module_info *mod);

/* 注销模块 */
int module_unregister(const char *name);

/* 获取模块 */
struct module_info *hu_module_get(const char *name);

/* 加载模块 */
int hu_module_load(const char *path);

/* 卸载模块 */
int hu_module_unload(const char *name);

/* 热更新模块 */
int module_hotupdate(const char *name, const char *new_path);

/* 创建检查点 */
int checkpoint_create(const char *description);

/* 恢复到检查点 */
int checkpoint_restore(uint64_t checkpoint_id);

/* 列出检查点 */
int checkpoint_list(struct update_checkpoint **checkpoints, int *count);

/* 删除检查点 */
int checkpoint_delete(uint64_t checkpoint_id);

/* 内核版本管理 */
typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    char build[16];
} kernel_version_t;

/* 获取当前内核版本 */
kernel_version_t kernel_get_version(void);

/* 设置内核版本 */
void kernel_set_version(uint32_t major, uint32_t minor, uint32_t patch, const char *build);

/* 检查更新 */
int kernel_check_update(const char *update_path, kernel_version_t *new_version);

/* 应用内核更新 */
int kernel_apply_update(const char *update_path);

/* ========== 安全函数替换 (ftrace-like 机制) ========== */

/* 函数替换记录 */
struct hu_func_replace {
    uint64_t target_addr;           /* 目标函数地址 */
    uint64_t original_code[4];      /* 原始指令 (最多 32 字节) */
    uint32_t original_size;         /* 原始指令大小 */
    uint64_t replacement_addr;      /* 替换函数地址 */
    uint32_t version;               /* 替换版本号 */
    int active;                     /* 是否激活 */
    char target_name[64];           /* 目标函数名 */
    char replacement_name[64];      /* 替换函数名 */
    struct hu_func_replace *next;   /* 链表下一个 */
};

/* 安装函数替换 (ftrace-like: 在目标函数入口插入跳转指令) */
int hu_func_replace_install(uint64_t target_addr, uint64_t replacement_addr,
                             const char *target_name, const char *replacement_name);

/* 卸载函数替换 (恢复原始指令) */
int hu_func_replace_uninstall(uint64_t target_addr);

/* 获取函数替换记录 */
struct hu_func_replace *hu_func_replace_get(uint64_t target_addr);

/* 列出所有函数替换 */
int hu_func_replace_list(struct hu_func_replace **list, int *count);

/* ========== 版本回滚 ========== */

/* 函数版本快照 (保存旧函数代码用于回滚) */
struct hu_version_snapshot {
    uint32_t version;               /* 版本号 */
    uint64_t timestamp;             /* 快照时间 */
    char description[64];           /* 描述 */
    struct hu_func_replace *replacements;  /* 该版本的所有替换 */
    int replacement_count;
    struct hu_version_snapshot *next;
};

/* 创建版本快照 (在应用更新前保存当前状态) */
int hu_version_snapshot_create(const char *description);

/* 回滚到指定版本 (恢复所有函数替换) */
int hu_version_rollback(uint32_t version);

/* 列出所有版本快照 */
int hu_version_list(struct hu_version_snapshot **list, int *count);

/* 删除版本快照 */
int hu_version_delete(uint32_t version);

/* 获取当前版本号 */
uint32_t hu_version_current(void);

/* ========== 依赖检查 ========== */

/* 符号依赖 */
struct hu_symbol_dep {
    char name[64];                  /* 符号名 */
    int required;                   /* 1=必需, 0=可选 */
    int found;                      /* 1=已找到, 0=未找到 */
};

/* 依赖检查结果 */
struct hu_dep_check_result {
    int all_satisfied;              /* 所有必需依赖是否满足 */
    int total_deps;                 /* 总依赖数 */
    int satisfied_deps;             /* 已满足依赖数 */
    int missing_required;           /* 缺失的必需依赖数 */
    int missing_optional;           /* 缺失的可选依赖数 */
    char missing_names[16][64];     /* 缺失的符号名 */
};

/* 检查符号是否在内核中可用 */
int hu_symbol_available(const char *name);

/* 批量检查依赖 */
int hu_check_dependencies(struct hu_symbol_dep *deps, int count,
                           struct hu_dep_check_result *result);

/* 注册内核符号 (供依赖查找) */
int hu_symbol_register(const char *name, uint64_t addr);

/* 查找内核符号地址 */
uint64_t hu_symbol_lookup(const char *name);

#endif
