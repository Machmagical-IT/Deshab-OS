/*
 * Zratro OS - User-Mode Driver Framework Implementation
 * 
 * @file umdf.c
 * @brief 用户态驱动框架核心实现
 * 
 * 实现特性:
 *   - 驱动运行在用户态，通过 IPC 与内核通信
 *   - 支持动态加载/卸载
 *   - Capability 安全控制硬件访问
 *   - 异步 I/O 支持
 */

#include "umdf.h"
#include "task.h"
#include "heap.h"
#include "string.h"
#include "console.h"
#include "capability.h"
#include "pexlf.h"
#include "vmm.h"

/* 简单的数字转字符串 */
static char *utoa(unsigned long n, char *buf, int base) {
    char *p = buf;
    do {
        *p++ = "0123456789ABCDEF"[n % base];
        n /= base;
    } while (n);
    *p = '\0';
    
    /* 反转 */
    char *q = buf;
    p--;
    while (q < p) {
        char t = *q;
        *q++ = *p;
        *p-- = t;
    }
    return buf;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 全局状态
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UMDF_MAX_DRIVERS        256
#define UMDF_MAX_DEVICES        1024
#define UMDF_CHANNEL_PREFIX     "umdf_"

/** 驱动表 */
static struct {
    uint64_t id;
    struct umdf_driver_info info;
    uint64_t pid;               /* 驱动进程 ID */
    uint64_t device_count;
    int state;
} driver_table[UMDF_MAX_DRIVERS];

static int driver_count = 0;
static spinlock_t driver_lock = SPINLOCK_INIT;

/** 设备表 */
static struct umdf_device device_table[UMDF_MAX_DEVICES];
/* static int device_count = 0; */  /* TODO: 实现设备计数 */
static spinlock_t device_lock = SPINLOCK_INIT;

/* ═══════════════════════════════════════════════════════════════════════════
 * 内部辅助函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 查找空闲驱动槽位
 */
static int find_free_driver_slot(void) {
    for (int i = 0; i < UMDF_MAX_DRIVERS; i++) {
        if (driver_table[i].id == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief 查找空闲设备槽位 (TODO: 实现)
 */
#if 0
static int find_free_device_slot(void) {
    for (int i = 0; i < UMDF_MAX_DEVICES; i++) {
        if (device_table[i].device_id == 0) {
            return i;
        }
    }
    return -1;
}
#endif

/**
 * @brief 生成唯一的驱动 ID
 */
static uint64_t generate_driver_id(void) {
    static uint64_t next_id = 1;
    return next_id++;
}

/**
 * @brief 生成唯一的设备 ID (TODO: 实现)
 */
#if 0
static uint64_t generate_device_id(void) {
    static uint64_t next_id = 1;
    return next_id++;
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * 驱动生命周期管理
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 加载用户态驱动
 * 
 * @param path 驱动文件路径
 * @param driver_id 输出的驱动 ID
 * @return int 0=成功，-1=失败
 * 
 * @note 驱动以独立进程形式运行
 *       通过 PEXLF 与内核通信
 */
int umdf_load_driver(const char *path, uint64_t *driver_id) {
    if (!path || !driver_id) {
        return -1;
    }
    
    spin_lock(&driver_lock);
    
    /* 检查容量 */
    if (driver_count >= UMDF_MAX_DRIVERS) {
        println("[UMDF] Driver table full");
        spin_unlock(&driver_lock);
        return -1;
    }
    
    int slot = find_free_driver_slot();
    if (slot < 0) {
        spin_unlock(&driver_lock);
        return -1;
    }
    
    /* TODO: 实际加载逻辑
     * 1. 读取驱动文件 (ELF/PE)
     * 2. 创建驱动进程
     * 3. 分配 capability (I/O 端口、DMA 等)
     * 4. 建立 IPC 通道
     */
    
    /* 简化实现：仅注册元数据 */
    driver_table[slot].id = generate_driver_id();
    strncpy(driver_table[slot].info.name, path, 31);
    driver_table[slot].info.type = UMDF_DRIVER_INVALID;  /* TODO: 检测类型 */
    driver_table[slot].state = UMDF_STATE_LOADING;
    driver_table[slot].pid = 0;  /* TODO: 实际进程 PID */
    driver_table[slot].device_count = 0;
    
    *driver_id = driver_table[slot].id;
    driver_count++;
    
    println("[UMDF] Driver loaded: ");
    println(path);
    
    spin_unlock(&driver_lock);
    return 0;
}

/**
 * @brief 卸载驱动
 */
int umdf_unload_driver(uint64_t driver_id) {
    if (driver_id == 0) {
        return -1;
    }
    
    spin_lock(&driver_lock);
    
    for (int i = 0; i < UMDF_MAX_DRIVERS; i++) {
        if (driver_table[i].id == driver_id) {
            /* 检查是否有打开的设备 */
            if (driver_table[i].device_count > 0) {
                println("[UMDF] Cannot unload: devices in use");
                spin_unlock(&driver_lock);
                return -1;
            }
            
            /* TODO: 终止驱动进程 */
            
            memset(&driver_table[i], 0, sizeof(driver_table[i]));
            driver_count--;
            
            spin_unlock(&driver_lock);
            return 0;
        }
    }
    
    spin_unlock(&driver_lock);
    return -1;
}

/**
 * @brief 启动驱动
 */
int umdf_start_driver(uint64_t driver_id) {
    spin_lock(&driver_lock);
    
    for (int i = 0; i < UMDF_MAX_DRIVERS; i++) {
        if (driver_table[i].id == driver_id) {
            if (driver_table[i].state != UMDF_STATE_LOADING) {
                spin_unlock(&driver_lock);
                return -1;
            }
            
            /* TODO: 启动驱动进程 */
            
            driver_table[i].state = UMDF_STATE_RUNNING;
            
            /* 创建 IPC 通道 */
            char channel_name[64];
            char num_buf[32];
            utoa(driver_id, num_buf, 10);
            
            strcpy(channel_name, UMDF_CHANNEL_PREFIX);
            strcat(channel_name, num_buf);
            pexlf_channel_create(channel_name);
            
            spin_unlock(&driver_lock);
            return 0;
        }
    }
    
    spin_unlock(&driver_lock);
    return -1;
}

/**
 * @brief 停止驱动
 */
int umdf_stop_driver(uint64_t driver_id) {
    spin_lock(&driver_lock);
    
    for (int i = 0; i < UMDF_MAX_DRIVERS; i++) {
        if (driver_table[i].id == driver_id) {
            if (driver_table[i].state != UMDF_STATE_RUNNING) {
                spin_unlock(&driver_lock);
                return -1;
            }
            
            /* TODO: 停止驱动进程 */
            
            driver_table[i].state = UMDF_STATE_SUSPENDED;
            
            spin_unlock(&driver_lock);
            return 0;
        }
    }
    
    spin_unlock(&driver_lock);
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 设备管理
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 枚举驱动管理的设备
 */
int umdf_enumerate_devices(uint64_t driver_id, struct umdf_device *devices, int max_count) {
    if (!devices || max_count <= 0) {
        return -1;
    }
    
    spin_lock(&device_lock);
    
    int count = 0;
    for (int i = 0; i < UMDF_MAX_DEVICES && count < max_count; i++) {
        if (device_table[i].device_id != 0 && 
            device_table[i].driver_id == driver_id) {
            devices[count++] = device_table[i];
        }
    }
    
    spin_unlock(&device_lock);
    return count;
}

/**
 * @brief 打开设备
 */
int umdf_open_device(uint64_t device_id, uint64_t *handle) {
    if (!handle) {
        return -1;
    }
    
    spin_lock(&device_lock);
    
    for (int i = 0; i < UMDF_MAX_DEVICES; i++) {
        if (device_table[i].device_id == device_id) {
            /* 检查 capability */
            if (cap_check_current(device_id, CAP_FS_READ) != 0) {
                println("[UMDF] Capability check failed");
                spin_unlock(&device_lock);
                return -1;
            }
            
            device_table[i].ref_count++;
            *handle = device_id;  /* 简化：handle = device_id */
            
            spin_unlock(&device_lock);
            return 0;
        }
    }
    
    spin_unlock(&device_lock);
    return -1;
}

/**
 * @brief 关闭设备
 */
int umdf_close_device(uint64_t handle) {
    spin_lock(&device_lock);
    
    for (int i = 0; i < UMDF_MAX_DEVICES; i++) {
        if (device_table[i].device_id == handle) {
            if (device_table[i].ref_count > 0) {
                device_table[i].ref_count--;
            }
            
            spin_unlock(&device_lock);
            return 0;
        }
    }
    
    spin_unlock(&device_lock);
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * I/O 操作
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 从设备读取数据
 */
int umdf_read(uint64_t handle, void *buf, size_t count, uint64_t offset) {
    if (!buf || count == 0) {
        return -1;
    }
    
    /* 验证用户缓冲区 */
    if (!access_ok(buf, count)) {
        return -1;
    }
    
    /* 构建请求 */
    struct umdf_request req = {0};
    req.request_id = handle;
    req.device_id = handle;
    req.op_code = UMDF_OP_READ;
    req.buffer_addr = (uint64_t)buf;
    req.buffer_size = count;
    req.offset = offset;
    req.length = count;
    
    /* 通过 IPC 发送给驱动 */
    int ret = pexlf_send("umdf_io", PEXLF_MSG_COMMAND, &req, sizeof(req));
    if (ret != 0) {
        return -1;
    }
    
    /* 等待响应 */
    struct umdf_response resp;
    ret = pexlf_recv("umdf_io", &resp, sizeof(resp), 1000);  /* 1s 超时 */
    if (ret < 0) {
        return -1;
    }
    
    return (int)resp.bytes_transferred;
}

/**
 * @brief 向设备写入数据
 */
int umdf_write(uint64_t handle, const void *buf, size_t count, uint64_t offset) {
    if (!buf || count == 0) {
        return -1;
    }
    
    /* 验证用户缓冲区 */
    if (!access_ok((void *)buf, count)) {
        return -1;
    }
    
    /* 构建请求 */
    struct umdf_request req = {0};
    req.request_id = handle;
    req.device_id = handle;
    req.op_code = UMDF_OP_WRITE;
    req.buffer_addr = (uint64_t)buf;
    req.buffer_size = count;
    req.offset = offset;
    req.length = count;
    
    /* 发送请求 */
    int ret = pexlf_send("umdf_io", PEXLF_MSG_COMMAND, &req, sizeof(req));
    if (ret != 0) {
        return -1;
    }
    
    /* 等待响应 */
    struct umdf_response resp;
    ret = pexlf_recv("umdf_io", &resp, sizeof(resp), 1000);
    if (ret < 0) {
        return -1;
    }
    
    return (int)resp.bytes_transferred;
}

/**
 * @brief 设备控制命令
 */
int umdf_ioctl(uint64_t handle, uint32_t code, void *arg) {
    /* TODO: 实现 ioctl */
    (void)handle;
    (void)code;
    (void)arg;
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 底层硬件访问 (需要 capability)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief MMIO 内存映射
 */
int umdf_mmio_map(uint64_t device_id, uint64_t addr, size_t size, void **mapped) {
    /* 检查 capability */
    if (cap_check_current(device_id, CAP_HW_MMIO) != 0) {
        println("[UMDF] CAP_HW_MMIO required");
        return -1;
    }
    
    /* TODO: 实际的 MMIO 映射 */
    (void)size;  /* 暂时未使用 */
    *mapped = (void *)addr;  /* 简化：直接返回物理地址 */
    return 0;
}

/**
 * @brief 释放 MMIO 映射
 */
int umdf_mmio_unmap(void *mapped) {
    /* TODO: 实际的 MMIO 释放 */
    (void)mapped;
    return 0;
}

/**
 * @brief 分配 DMA 缓冲区
 */
int umdf_dma_alloc(size_t size, void **dma_buf, uint64_t *dma_addr) {
    /* 检查 capability */
    if (cap_check_current(0, CAP_HW_DMA) != 0) {
        println("[UMDF] CAP_HW_DMA required");
        return -1;
    }
    
    /* TODO: 实际的 DMA 分配 */
    *dma_buf = kmalloc(size);
    *dma_addr = (uint64_t)*dma_buf;  /* 简化：虚拟地址=物理地址 */
    
    if (!*dma_buf) {
        return -1;
    }
    
    return 0;
}

/**
 * @brief 释放 DMA 缓冲区
 */
int umdf_dma_free(void *dma_buf, size_t size) {
    (void)size;  /* TODO: 使用 size 进行验证 */
    kfree(dma_buf);
    return 0;
}

/**
 * @brief 注册中断处理程序
 */
int umdf_register_interrupt(uint64_t irq, void (*handler)(void *), void *data) {
    /* 检查 capability */
    if (cap_check_current(0, CAP_HW_IO) != 0) {
        println("[UMDF] CAP_HW_IO required for interrupt");
        return -1;
    }
    
    /* TODO: 实际的中断注册 */
    (void)irq;
    (void)handler;
    (void)data;
    return 0;
}

/**
 * @brief 注销中断
 */
int umdf_unregister_interrupt(uint64_t irq) {
    /* TODO: 实际的中断注销 */
    (void)irq;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 查询接口
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 获取驱动信息
 */
int umdf_get_driver_info(uint64_t driver_id, struct umdf_driver_info *info) {
    if (!info) {
        return -1;
    }
    
    spin_lock(&driver_lock);
    
    for (int i = 0; i < UMDF_MAX_DRIVERS; i++) {
        if (driver_table[i].id == driver_id) {
            *info = driver_table[i].info;
            spin_unlock(&driver_lock);
            return 0;
        }
    }
    
    spin_unlock(&driver_lock);
    return -1;
}

/**
 * @brief 获取设备信息
 */
int umdf_get_device_info(uint64_t device_id, struct umdf_device *info) {
    if (!info) {
        return -1;
    }
    
    spin_lock(&device_lock);
    
    for (int i = 0; i < UMDF_MAX_DEVICES; i++) {
        if (device_table[i].device_id == device_id) {
            *info = device_table[i];
            spin_unlock(&device_lock);
            return 0;
        }
    }
    
    spin_unlock(&device_lock);
    return -1;
}

/**
 * @brief 列出所有驱动
 */
int umdf_list_drivers(uint64_t *ids, int max_count) {
    if (!ids || max_count <= 0) {
        return -1;
    }
    
    spin_lock(&driver_lock);
    
    int count = 0;
    for (int i = 0; i < UMDF_MAX_DRIVERS && count < max_count; i++) {
        if (driver_table[i].id != 0) {
            ids[count++] = driver_table[i].id;
        }
    }
    
    spin_unlock(&driver_lock);
    return count;
}

/**
 * @brief 列出所有设备
 */
int umdf_list_devices(uint64_t *ids, int max_count) {
    if (!ids || max_count <= 0) {
        return -1;
    }
    
    spin_lock(&device_lock);
    
    int count = 0;
    for (int i = 0; i < UMDF_MAX_DEVICES && count < max_count; i++) {
        if (device_table[i].device_id != 0) {
            ids[count++] = device_table[i].device_id;
        }
    }
    
    spin_unlock(&device_lock);
    return count;
}
