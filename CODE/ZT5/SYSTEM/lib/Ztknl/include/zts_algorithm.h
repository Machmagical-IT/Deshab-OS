#ifndef ZTS_ALGORITHM_H
#define ZTS_ALGORITHM_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * ZTS7 Dynamic Algorithm System
 * ZratroOS 动态算法模块系统
 * ============================================================ */

#define ZTS_MAGIC         0x53545A00
#define ZTS_API_VERSION   1
#define ZTS_UUID_LEN      16
#define ZTS_MAX_NAME      64
#define ZTS_MAX_DESC      256
#define ZTS_MAX_AUTHOR    64
#define ZTS_MAX_LICENSE   32
#define ZTS_MAX_MODULES   64
#define ZTS_MAX_PATH      256
#define ZTS_MAX_DEPS      16

/* 模块分类 */
enum zts_category {
    ZTS_CAT_FILESYSTEM = 0,
    ZTS_CAT_NETWORK,
    ZTS_CAT_CRYPTO,
    ZTS_CAT_COMPRESSION,
    ZTS_CAT_SCHEDULER,
    ZTS_CAT_MEMORY,
    ZTS_CAT_DRIVER,
    ZTS_CAT_PATCH,
    ZTS_CAT_INJECT,
    ZTS_CAT_CUSTOM,
};

/* 模块执行模式 */
enum zts_mode {
    ZTS_MODE_API = 0,     /* 通过内核 API 交互 */
    ZTS_MODE_PATCH,       /* 直接修改内核代码 */
    ZTS_MODE_INJECT,      /* 插入钩子 */
};

/* .zts 文件头格式 */
struct zts_header {
    uint32_t magic;
    uint16_t version;
    uint16_t api_min_version;
    char name[ZTS_MAX_NAME];
    char description[ZTS_MAX_DESC];
    char author[ZTS_MAX_AUTHOR];
    char license[ZTS_MAX_LICENSE];
    uint8_t uuid[ZTS_UUID_LEN];
    enum zts_category category;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t patch_version;
    uint32_t num_dependencies;
    uint64_t init_func_offset;
    uint64_t cleanup_func_offset;
    uint64_t execute_func_offset;
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t checksum;
    uint8_t signature[256];
} __attribute__((packed));

/* ZTS API 表 - 内核提供给模块的函数 */
struct zts_api_table {
    uint32_t api_version;

    /* 内存管理 */
    void *(*alloc)(size_t size);
    void (*free)(void *ptr);
    void *(*realloc)(void *ptr, size_t size);

    /* 日志 */
    void (*log_print)(const char *level, const char *fmt, ...);

    /* 时间 */
    uint64_t (*time_now)(void);

    /* VFS 操作 (可选) */
    int (*vfs_open)(const char *path, uint32_t mode);
    int (*vfs_close)(int fd);
    int (*vfs_read)(int fd, void *buf, size_t count);
    int (*vfs_write)(int fd, const void *buf, size_t count);

    /* 网络 (可选) */
    int (*net_send)(const void *data, size_t len);
    int (*net_recv)(void *data, size_t len);

    /* 字符串操作 */
    int (*strcmp)(const char *s1, const char *s2);
    void *(*memcpy)(void *dst, const void *src, size_t n);
    void *(*memset)(void *s, int c, size_t n);
};

/* 运行时模块结构 */
struct zts_module {
    char name[ZTS_MAX_NAME];
    uint8_t uuid[ZTS_UUID_LEN];
    enum zts_category category;
    uint32_t major_version;
    uint32_t minor_version;

    /* 模块代码地址 */
    void *code_base;          /* 模块代码基址 */
    size_t code_size;         /* 代码大小 */

    /* 入口点 */
    int (*init)(struct zts_api_table *api);
    void (*cleanup)(void);
    int (*execute)(void *input, void *output, void *ctx);

    /* 模块数据 */
    void *data;
    size_t data_size;

    /* 状态 */
    int loaded;
    int initialized;
    int priority;             /* 加载优先级 (从文件名解析) */

    /* Patch/Inject 模式专用 */
    void *patch_address;
    size_t patch_size;
    uint8_t *patch_original;  /* 保存原始代码用于恢复 */
};

/* ZTS 系统状态 */
struct zts_system {
    struct zts_api_table api;
    struct zts_module modules[ZTS_MAX_MODULES];
    int module_count;
    int initialized;
    char load_path[ZTS_MAX_PATH];
};

/* ============================================================
 * 模块声明宏 (供 .zts.c 文件使用)
 * ============================================================ */

/* 声明模块元数据 */
#define ZTS_DECLARE(name, desc, author, lic, cat, major, minor) \
    __attribute__((section(".zts_header"))) \
    static struct zts_header zts_header = { \
        .magic = ZTS_MAGIC, \
        .version = ZTS_API_VERSION, \
        .api_min_version = 1, \
        .name = name, \
        .description = desc, \
        .author = author, \
        .license = lic, \
        .category = cat, \
        .major_version = major, \
        .minor_version = minor, \
    }

/* 标记入口点 */
#define ZTS_MODULE_INIT(name) \
    __attribute__((section(".init.text"))) \
    int name(struct zts_api_table *api)

#define ZTS_MODULE_EXECUTE(name) \
    __attribute__((section(".text"))) \
    int name(void *input, void *output, void *ctx)

#define ZTS_MODULE_CLEANUP(name) \
    __attribute__((section(".exit.text"))) \
    void name(void)

/* ============================================================
 * 内核 API 函数
 * ============================================================ */

/* 初始化 ZTS 系统 */
int zts_init(void);

/* 加载模块 */
int zts_load_module(const char *path);

/* 加载目录下的所有模块 */
int zts_load_directory(const char *dir_path);

/* 执行模块 */
int zts_execute_module(const char *name, void *input, void *output);

/* 卸载模块 */
int zts_unload_module(const char *name);

/* 按 UUID 查找模块 */
struct zts_module *zts_find_by_uuid(const uint8_t uuid[ZTS_UUID_LEN]);

/* 按名称查找模块 */
struct zts_module *zts_find_by_name(const char *name);

/* 打印加载的模块列表 */
void zts_list_modules(void);

/* 获取 API 表 */
struct zts_api_table *zts_get_api(void);

/* 恢复 Patch (Patch 模式专用) */
void zts_restore_patch(struct zts_module *mod);

#endif /* ZTS_ALGORITHM_H */
