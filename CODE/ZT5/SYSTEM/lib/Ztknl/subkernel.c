#include "subkernel.h"
#include "console.h"
#include "vfs.h"
#include "heap.h"
#include "string.h"

/* 子内核注册表 */
static struct subkernel_info *subkernel_registry[MAX_SUBKERNELS];
static int subkernel_count = 0;

/* Windows 兼容层实例 */
static struct windows_compat_layer windows_layer;

/* Linux 兼容层实例 */
static struct linux_compat_layer linux_layer;

void subkernel_init(void) {
    /* 清空注册表 */
    for (int i = 0; i < MAX_SUBKERNELS; i++) {
        subkernel_registry[i] = NULL;
    }
    subkernel_count = 0;
}

int subkernel_register(struct subkernel_info *sk) {
    if (sk == NULL || subkernel_count >= MAX_SUBKERNELS) {
        return -1;
    }
    
    /* 检查是否已注册 */
    for (int i = 0; i < MAX_SUBKERNELS; i++) {
        if (subkernel_registry[i] != NULL &&
            subkernel_registry[i]->type == sk->type) {
            print("[SUBKERNEL] Already registered: ");
            println(sk->name);
            return -1;
        }
    }
    
    /* 查找空闲槽 */
    int slot = -1;
    for (int i = 0; i < MAX_SUBKERNELS; i++) {
        if (subkernel_registry[i] == NULL) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        return -1;
    }
    
    subkernel_registry[slot] = sk;
    subkernel_count++;
    
    print("[SUBKERNEL] Registered: ");
    println(sk->name);
    
    /* 调用初始化函数 */
    if (sk->init != NULL) {
        if (sk->init() != 0) {
            print("[SUBKERNEL] Init failed: ");
            println(sk->name);
            subkernel_registry[slot] = NULL;
            subkernel_count--;
            return -1;
        }
    }
    
    sk->state = SUBKERNEL_STATE_RUNNING;
    
    return 0;
}

int subkernel_unregister(subkernel_type_t type) {
    for (int i = 0; i < MAX_SUBKERNELS; i++) {
        if (subkernel_registry[i] != NULL &&
            subkernel_registry[i]->type == type) {
            
            struct subkernel_info *sk = subkernel_registry[i];
            
            /* 调用关闭函数 */
            if (sk->shutdown != NULL) {
                sk->shutdown();
            }
            
            subkernel_registry[i] = NULL;
            subkernel_count--;
            
            print("[SUBKERNEL] Unregistered: ");
            println(sk->name);
            
            return 0;
        }
    }
    return -1;
}

struct subkernel_info *subkernel_get(subkernel_type_t type) {
    for (int i = 0; i < MAX_SUBKERNELS; i++) {
        if (subkernel_registry[i] != NULL &&
            subkernel_registry[i]->type == type) {
            return subkernel_registry[i];
        }
    }
    return NULL;
}

int subkernel_load(subkernel_type_t type, const char *path) {
    /* TODO: 加载子内核镜像 */
    (void)type;
    (void)path;
    
    print("[SUBKERNEL] Loading from: ");
    println(path);
    
    /* 1. 读取镜像文件 */
    /* 2. 验证镜像格式 */
    /* 3. 重定位并加载 */
    /* 4. 调用初始化 */
    
    return -1;  /* 尚未实现 */
}

int subkernel_unload(subkernel_type_t type) {
    return subkernel_unregister(type);
}

int subkernel_suspend(subkernel_type_t type) {
    struct subkernel_info *sk = subkernel_get(type);
    if (sk == NULL) {
        return -1;
    }
    
    sk->state = SUBKERNEL_STATE_SUSPENDED;
    print("[SUBKERNEL] Suspended: ");
    println(sk->name);
    
    return 0;
}

int subkernel_resume(subkernel_type_t type) {
    struct subkernel_info *sk = subkernel_get(type);
    if (sk == NULL) {
        return -1;
    }
    
    sk->state = SUBKERNEL_STATE_RUNNING;
    print("[SUBKERNEL] Resumed: ");
    println(sk->name);
    
    return 0;
}

int subkernel_dispatch_syscall(abi_type_t abi, uint64_t num, uint64_t *args, uint64_t *ret) {
    /* 根据ABI类型找到对应的子内核 */
    subkernel_type_t type = SUBKERNEL_NONE;
    
    switch (abi) {
        case ABI_LINUX:
            type = SUBKERNEL_LINUX;
            break;
        case ABI_WINDOWS:
            type = SUBKERNEL_WINDOWS;
            break;
        case ABI_FREEBSD:
            type = SUBKERNEL_FREEBSD;
            break;
        default:
            return -1;  /* 使用原生 Zratro 处理 */
    }
    
    struct subkernel_info *sk = subkernel_get(type);
    if (sk == NULL || sk->state != SUBKERNEL_STATE_RUNNING) {
        return -1;
    }
    
    if (!(sk->capabilities & SUBKERNEL_CAP_SYSCALL)) {
        return -1;
    }
    
    if (sk->syscall == NULL) {
        return -1;
    }
    
    return sk->syscall(num, args);
}

/* 热更新实现 */
int subkernel_hotupdate(subkernel_type_t type, const char *new_path) {
    struct subkernel_info *sk = subkernel_get(type);
    if (sk == NULL) {
        return -1;
    }
    
    if (!(sk->capabilities & SUBKERNEL_CAP_HOTUPDATE)) {
        print("[SUBKERNEL] Hot update not supported: ");
        println(sk->name);
        return -1;
    }
    
    print("[SUBKERNEL] Hot update starting: ");
    println(sk->name);
    
    /* 1. 准备更新 */
    if (sk->update_prepare != NULL) {
        if (sk->update_prepare() != 0) {
            println("[SUBKERNEL] Prepare failed");
            return -1;
        }
    }
    
    /* 2. 读取新镜像 */
    size_t size;
    void *new_image = vfs_read_file(new_path, &size);
    if (new_image == NULL) {
        println("[SUBKERNEL] Failed to read new image");
        return -1;
    }
    
    /* 3. 应用更新 */
    if (sk->update_apply == NULL) {
        kfree(new_image);
        println("[SUBKERNEL] No update_apply callback, cannot apply update");
        return -1;
    }
    
    if (sk->update_apply(new_image, size) != 0) {
        kfree(new_image);
        println("[SUBKERNEL] Apply failed, rolling back...");
        if (sk->update_rollback) {
            sk->update_rollback();
        }
        return -1;
    }
    
    kfree(new_image);
    
    print("[SUBKERNEL] Hot update complete: ");
    println(sk->name);
    
    return 0;
}

int subkernel_hotupdate_rollback(subkernel_type_t type) {
    struct subkernel_info *sk = subkernel_get(type);
    if (sk == NULL || sk->update_rollback == NULL) {
        return -1;
    }
    
    sk->update_rollback();
    
    print("[SUBKERNEL] Rollback complete: ");
    println(sk->name);
    
    return 0;
}

/* Windows 兼容层接口 */
struct windows_compat_layer *subkernel_get_windows(void) {
    struct subkernel_info *sk = subkernel_get(SUBKERNEL_WINDOWS);
    if (sk == NULL) {
        return NULL;
    }
    return &windows_layer;
}

/* Linux 兼容层接口 */
struct linux_compat_layer *subkernel_get_linux(void) {
    struct subkernel_info *sk = subkernel_get(SUBKERNEL_LINUX);
    if (sk == NULL) {
        return NULL;
    }
    return &linux_layer;
}
