/*
 * Zratro OS - Kernel Module System
 * 
 * 可加载内核模块支持
 */

#ifndef MODULE_H
#define MODULE_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "string.h"
#include "rcu.h"

/* ========== 模块标志 ========== */

#define MODULE_FLAG_LOADED      (1 << 0)    /* 已加载 */
#define MODULE_FLAG_LIVE        (1 << 1)    /* 正在运行 */
#define MODULE_FLAG_COMING      (1 << 2)    /* 正在加载 */
#define MODULE_FLAG_GOING       (1 << 3)    /* 正在卸载 */
#define MODULE_FLAG_UNPROTECTED (1 << 4)    /* 无保护 */
#define MODULE_FLAG_SIG_OK      (1 << 5)    /* 签名验证通过 */
#define MODULE_FLAG_FORCED      (1 << 6)    /* 强制加载 */

/* ========== 模块导出符号 ========== */

struct kernel_symbol {
    uint64_t value;                 /* 符号地址 */
    const char *name;               /* 符号名称 */
    const char *namespace;          /* 命名空间 */
    struct module *module;          /* 所属模块 */
    struct list_head list;          /* 链表 */
};

/* ========== 模块依赖 ========== */

struct module_dep {
    struct module *module;          /* 依赖的模块 */
    uint32_t count;                 /* 引用计数 */
    struct list_head list;          /* 链表 */
};

/* ========== 模块内存区域 ========== */

struct module_memory {
    void *base;                     /* 基地址 */
    size_t size;                    /* 大小 */
    uint64_t flags;                 /* 标志 */
    uint64_t phys;                  /* 物理地址 */
};

#define MOD_TEXT    0   /* 代码段 */
#define MOD_DATA    1   /* 数据段 */
#define MOD_RODATA  2   /* 只读数据 */
#define MOD_RO_AFTER_INIT 3  /* init 后只读 */

/* ========== 模块属性 ========== */

struct module_attribute {
    const char *name;
    int (*show)(struct module *mod, char *buf);
    int (*store)(struct module *mod, const char *buf, size_t count);
    void *data;
    struct list_head list;
};

/* ========== 模块参数 ========== */

struct module_param {
    const char *name;               /* 参数名 */
    void *addr;                     /* 地址 */
    const char *type;               /* 类型 */
    uint16_t flags;                 /* 标志 */
    void (*set)(void *val);         /* 设置函数 */
    int (*get)(void *val, char *buf); /* 获取函数 */
    struct list_head list;
};

/* 参数权限 */
#define MODULE_PARAM_PARM       0   /* 启动参数 */
#define MODULE_PARAM_READ       0400
#define MODULE_PARAM_WRITE      0200
#define MODULE_PARAM_EXEC       0100

/* ========== 模块结构 ========== */

struct module {
    /* 基本信息 */
    char name[64];                  /* 模块名称 */
    char version[64];               /* 版本 */
    char license[64];               /* 许可证 */
    char author[64];                /* 作者 */
    char description[256];          /* 描述 */
    char srcversion[64];            /* 源版本 */
    
    /* 状态 */
    int state;                      /* 模块状态 */
    uint64_t flags;                 /* 标志 */
    
    /* 内存区域 */
    struct module_memory mem[4];    /* TEXT/DATA/RODATA/RO_AFTER_INIT */
    size_t core_size;               /* 核心大小 */
    size_t init_size;               /* 初始化段大小 */
    
    /* 符号表 */
    struct kernel_symbol *syms;     /* 导出符号 */
    uint32_t num_syms;              /* 符号数 */
    uint64_t *crcs;                 /* 符号 CRC */
    
    /* 未定义符号 */
    struct kernel_symbol *unresolved;
    uint32_t num_unresolved;
    int *unresolved_gpl;
    
    /* 依赖 */
    struct list_head dependencies;  /* 依赖列表 */
    struct list_head dependents;    /* 被依赖列表 */
    uint32_t ref_count;             /* 引用计数 */
    
    /* 参数 */
    struct list_head params;        /* 参数列表 */
    
    /* 属性 */
    struct list_head attrs;         /* 属性列表 */
    
    /* 入口点 */
    int (*init)(void);              /* 初始化函数 */
    void (*exit)(void);             /* 清理函数 */
    void (*cleanup)(void);          /* 清理函数 (内部) */
    
    /* 异步初始化 */
    int async_init;                 /* 是否异步初始化 */
    int init_err;                   /* 初始化错误 */
    
    /* ELF 信息 */
    void *elf_info;                 /* ELF 信息 */
    size_t elf_info_size;
    
    /* 持久数据 */
    void *percpu;                   /* Per-CPU 数据 */
    size_t percpu_size;
    
    /* 链表 */
    struct list_head list;          /* 全局模块链表 */
    struct rcu_head rcu;            /* RCU 头 */
    
    /* 锁 */
    spinlock_t lock;
};

/* 模块状态 */
enum module_state {
    MODULE_STATE_UNLOADED = 0,
    MODULE_STATE_LOADING,
    MODULE_STATE_LIVE,
    MODULE_STATE_COMING,
    MODULE_STATE_GOING,
};

/* ========== 模块声明宏 ========== */

/* 模块许可证 */
#define MODULE_LICENSE(license) \
    static const char __module_license[] __attribute__((section(".modinfo"))) = "license=" license

/* 模块作者 */
#define MODULE_AUTHOR(author) \
    static const char __module_author[] __attribute__((section(".modinfo"))) = "author=" author

/* 模块描述 */
#define MODULE_DESCRIPTION(desc) \
    static const char __module_description[] __attribute__((section(".modinfo"))) = "description=" desc

/* 模块版本 */
#define MODULE_VERSION(version) \
    static const char __module_version[] __attribute__((section(".modinfo"))) = "version=" version

/* 模块别名 */
#define MODULE_ALIAS(alias) \
    static const char __module_alias[] __attribute__((section(".modinfo"))) = "alias=" alias

/* 模块设备表 */
#define MODULE_DEVICE_TABLE(type, name) \
    static const char __module_device_table[] __attribute__((section(".modinfo"))) = "device_table=" #type "," #name

/* 模块初始化 */
#define module_init(initfn) \
    static int (*__initcall)(void) __attribute__((section(".init.text"))) = initfn; \
    static const char __module_init[] __attribute__((section(".modinfo"))) = "init=" #initfn

/* 模块退出 */
#define module_exit(exitfn) \
    static void (*__exitcall)(void) __attribute__((section(".exit.text"))) = exitfn; \
    static const char __module_exit[] __attribute__((section(".modinfo"))) = "exit=" #exitfn

/* ========== 参数宏 ========== */

#define module_param(name, type, perm) \
    static struct module_param __param_##name = { \
        .name = #name, \
        .addr = &name, \
        .type = #type, \
        .flags = perm, \
    }; \
    static const char __param_desc_##name[] __attribute__((section(".modinfo"))) = "parm=" #name ":" #type

#define module_param_named(name, value, type, perm) \
    static struct module_param __param_##name = { \
        .name = #name, \
        .addr = &value, \
        .type = #type, \
        .flags = perm, \
    }

#define module_param_string(name, string, len, perm) \
    static struct module_param __param_##name = { \
        .name = #name, \
        .addr = string, \
        .type = "string", \
        .flags = perm, \
    }

#define module_param_array(name, type, nump, perm) \
    static struct module_param __param_##name = { \
        .name = #name, \
        .addr = name, \
        .type = #type, \
        .flags = perm, \
    }

/* ========== 导出符号宏 ========== */

#define EXPORT_SYMBOL(sym) \
    static struct kernel_symbol __ksymtab_##sym = { \
        .value = (uint64_t)&sym, \
        .name = #sym, \
        .module = THIS_MODULE, \
    }; \
    static const char __kstrtab_##sym[] __attribute__((section(".kstrtab"))) = #sym

#define EXPORT_SYMBOL_GPL(sym) \
    static struct kernel_symbol __ksymtab_gpl_##sym = { \
        .value = (uint64_t)&sym, \
        .name = #sym, \
        .module = THIS_MODULE, \
    }

#define THIS_MODULE ((struct module *)0)

/* ========== 模块注册 ========== */

/* 驱动注册 */
struct device_driver {
    const char *name;
    const char *bus_name;
    struct module *owner;
    
    int (*probe)(struct device *dev);
    int (*remove)(struct device *dev);
    void (*shutdown)(struct device *dev);
    int (*suspend)(struct device *dev);
    int (*resume)(struct device *dev);
    
    struct list_head list;
};

#define module_driver(driver, register, unregister) \
    static int __init driver##_init(void) { \
        return register(&driver); \
    } \
    static void __exit driver##_exit(void) { \
        unregister(&driver); \
    } \
    module_init(driver##_init); \
    module_exit(driver##_exit)

/* ========== 函数原型 ========== */

/* 初始化 */
int module_init_system(void);

/* 加载/卸载 */
struct module *module_load(const char *name, const void *data, size_t size);
int module_unload(struct module *mod);
int module_unload_name(const char *name);

/* 查找 */
struct module *module_find(const char *name);
struct module *module_find_by_addr(uint64_t addr);

/* 符号 */
struct kernel_symbol *module_find_symbol(const char *name);
int module_add_symbol(struct module *mod, const char *name, uint64_t value);
void *module_get_symbol(struct module *mod, const char *name);

/* 引用 */
struct module *module_get(struct module *mod);
void module_put(struct module *mod);
int module_ref_count(struct module *mod);

/* 参数 */
int module_param_set(struct module *mod, const char *name, const char *value);
int module_param_get(struct module *mod, const char *name, char *buf, size_t size);

/* 属性 */
int module_add_attr(struct module *mod, struct module_attribute *attr);
struct module_attribute *module_find_attr(struct module *mod, const char *name);

/* 系统调用 */
int sys_init_module(void *module_image, size_t len, const char *param_values);
int sys_finit_module(int fd, const char *param_values, int flags);
int sys_delete_module(const char *name, int flags);

/* 列表 */
struct list_head *module_get_list(void);

/* 遍历 */
#define for_each_module(mod) \
    list_for_each_entry(mod, &module_list, list)

/* ========== 辅助函数 ========== */

static inline int module_is_live(struct module *mod) {
    return mod && mod->state == MODULE_STATE_LIVE;
}

static inline int module_is_loading(struct module *mod) {
    return mod && mod->state == MODULE_STATE_LOADING;
}

static inline int module_is_coming(struct module *mod) {
    return mod && mod->state == MODULE_STATE_COMING;
}

static inline int module_is_going(struct module *mod) {
    return mod && mod->state == MODULE_STATE_GOING;
}

/* 许可证检查 */
static inline int module_license_is_gpl(const char *license) {
    return (strcmp(license, "GPL") == 0 ||
            strcmp(license, "GPL v2") == 0 ||
            strcmp(license, "GPL and additional rights") == 0 ||
            strcmp(license, "Dual BSD/GPL") == 0 ||
            strcmp(license, "Dual MIT/GPL") == 0 ||
            strcmp(license, "Dual MPL/GPL") == 0);
}

#endif /* MODULE_H */
