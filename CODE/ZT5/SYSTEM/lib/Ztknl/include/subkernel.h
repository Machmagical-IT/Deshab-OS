#ifndef SUBKERNEL_H
#define SUBKERNEL_H

#include <stdint.h>
#include "abi.h"

/* 子内核类型 */
typedef enum {
    SUBKERNEL_NONE = 0,
    SUBKERNEL_LINUX,        /* Linux 内核模块 */
    SUBKERNEL_WINDOWS,      /* Windows NT 兼容层 */
    SUBKERNEL_FREEBSD,      /* FreeBSD 兼容层 */
    SUBKERNEL_CUSTOM,       /* 自定义子内核 */
} subkernel_type_t;

/* 子内核状态 */
typedef enum {
    SUBKERNEL_STATE_UNLOADED = 0,
    SUBKERNEL_STATE_LOADING,
    SUBKERNEL_STATE_RUNNING,
    SUBKERNEL_STATE_SUSPENDED,
    SUBKERNEL_STATE_ERROR,
} subkernel_state_t;

/* 子内核能力标志 */
#define SUBKERNEL_CAP_SYSCALL       (1 << 0)    /* 可处理系统调用 */
#define SUBKERNEL_CAP_DRIVER        (1 << 1)    /* 包含硬件驱动 */
#define SUBKERNEL_CAP_FS            (1 << 2)    /* 包含文件系统 */
#define SUBKERNEL_CAP_NETWORK       (1 << 3)    /* 包含网络栈 */
#define SUBKERNEL_CAP_GUI           (1 << 4)    /* 包含图形界面 */
#define SUBKERNEL_CAP_HOTUPDATE     (1 << 5)    /* 支持热更新 */

/* 子内核信息结构 */
struct subkernel_info {
    subkernel_type_t type;              /* 类型 */
    subkernel_state_t state;            /* 状态 */
    uint32_t capabilities;              /* 能力标志 */
    
    char name[32];                      /* 名称 */
    char version[16];                   /* 版本号 */
    
    uint64_t base_addr;                 /* 加载基地址 */
    uint64_t size;                      /* 大小 */
    
    /* 接口函数指针 */
    int (*init)(void);                  /* 初始化 */
    int (*syscall)(uint64_t num, uint64_t *args);  /* 系统调用处理 */
    int (*interrupt)(uint64_t vector, void *frame); /* 中断处理 */
    void (*shutdown)(void);             /* 关闭 */
    
    /* 热更新接口 */
    int (*update_prepare)(void);        /* 准备更新 */
    int (*update_apply)(void *new_image, size_t size); /* 应用更新 */
    void (*update_rollback)(void);      /* 回滚更新 */
    
    void *private_data;                 /* 私有数据 */
};

/* 子内核注册表 */
#define MAX_SUBKERNELS  8

/* 初始化子系统 */
void subkernel_init(void);

/* 注册子内核 */
int subkernel_register(struct subkernel_info *sk);

/* 注销子内核 */
int subkernel_unregister(subkernel_type_t type);

/* 获取子内核 */
struct subkernel_info *subkernel_get(subkernel_type_t type);

/* 加载子内核镜像 */
int subkernel_load(subkernel_type_t type, const char *path);

/* 卸载子内核 */
int subkernel_unload(subkernel_type_t type);

/* 挂起/恢复子内核 */
int subkernel_suspend(subkernel_type_t type);
int subkernel_resume(subkernel_type_t type);

/* 通过子内核处理系统调用 */
int subkernel_dispatch_syscall(abi_type_t abi, uint64_t num, uint64_t *args, uint64_t *ret);

/* 热更新接口 */
int subkernel_hotupdate(subkernel_type_t type, const char *new_path);
int subkernel_hotupdate_rollback(subkernel_type_t type);

/* Windows NT 兼容层特化接口 */
struct windows_compat_layer {
    /* PE 加载器 */
    int (*pe_load)(const char *path, void **entry);
    
    /* Windows API 映射 */
    int (*winapi_dispatch)(const char *name, void *args);
    
    /* 注册表模拟 */
    int (*reg_open)(const char *path, void **key);
    int (*reg_read)(void *key, const char *value, void *data, size_t *size);
    int (*reg_write)(void *key, const char *value, const void *data, size_t size);
    int (*reg_close)(void *key);
    
    /* NT 内核对象 */
    void *(*object_create)(const char *type, void *attrs);
    int (*object_destroy)(void *object);
};

/* Linux 内核模块特化接口 */
struct linux_compat_layer {
    /* ELF 加载 (使用 Linux ABI) */
    int (*elf_load_linux)(const char *path, void **entry);
    
    /* /proc 文件系统模拟 */
    int (*proc_create)(const char *name, void *ops);
    int (*proc_remove)(const char *name);
    
    /* sysfs 模拟 */
    int (*sysfs_create)(const char *path, void *attrs);
    int (*sysfs_remove)(const char *path);
    
    /* 设备模型 */
    int (*device_register)(const char *name, void *dev);
    int (*device_unregister)(const char *name);
};

/* 获取 Windows 兼容层 */
struct windows_compat_layer *subkernel_get_windows(void);

/* 获取 Linux 兼容层 */
struct linux_compat_layer *subkernel_get_linux(void);

#endif
