/*
 * Zratro OS - User-Mode Driver Framework (UMDF)
 * 
 * 用户态驱动框架 - 超越 Linux 的驱动架构
 * 
 * @file umdf.h
 * @brief 用户态驱动框架头文件
 * 
 * 核心理念:
 *   - 驱动运行在用户态，崩溃不影响内核
 *   - 通过 PEXLF IPC 与内核通信
 *   - 支持热插拔和动态加载
 *   - Capability 安全控制
 * 
 * 对比 Linux:
 *   - Linux: 内核态驱动为主，崩溃导致 kernel panic
 *   - ZratroOS: 用户态驱动为主，崩溃只影响进程
 * 
 * @author ZratroOS Team
 * @version 1.0
 * @date 2026
 */

#ifndef UMDF_H
#define UMDF_H

#include "ktypes.h"
#include "pexlf.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 驱动类型定义
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 驱动类型枚举
 */
typedef enum {
    UMDF_DRIVER_INVALID   = 0,
    UMDF_DRIVER_BLOCK     = 1,    /* 块设备驱动 (磁盘等) */
    UMDF_DRIVER_CHAR      = 2,    /* 字符设备驱动 (串口等) */
    UMDF_DRIVER_NETWORK   = 3,    /* 网络设备驱动 */
    UMDF_DRIVER_USB       = 4,    /* USB 驱动 */
    UMDF_DRIVER_PCI       = 5,    /* PCI 驱动 */
    UMDF_DRIVER_GPU       = 6,    /* 显卡驱动 */
    UMDF_DRIVER_AUDIO     = 7,    /* 音频驱动 */
    UMDF_DRIVER_INPUT     = 8,    /* 输入设备驱动 */
    UMDF_DRIVER_SENSOR    = 9,    /* 传感器驱动 */
} umdf_driver_type_t;

/**
 * @brief 驱动状态
 */
typedef enum {
    UMDF_STATE_UNLOADED   = 0,    /* 未加载 */
    UMDF_STATE_LOADING    = 1,    /* 加载中 */
    UMDF_STATE_RUNNING    = 2,    /* 运行中 */
    UMDF_STATE_SUSPENDED  = 3,    /* 已挂起 */
    UMDF_STATE_ERROR      = 4,    /* 错误状态 */
    UMDF_STATE_UNLOADING  = 5,    /* 卸载中 */
} umdf_state_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * 驱动描述符结构
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 驱动元数据信息 (64 字节)
 */
struct umdf_driver_info {
    char name[32];                /* 驱动名称 */
    char version[16];             /* 版本号 */
    char author[32];              /* 作者信息 */
    
    uint32_t type;                /* umdf_driver_type_t */
    uint32_t flags;               /* 标志位 */
    
    uint64_t device_class;        /* 支持的设备类别 */
    uint64_t vendor_id;           /* 厂商 ID (PCI/USB) */
    uint64_t device_id;           /* 设备 ID */
    
    uint64_t entry_point;         /* 入口函数地址 */
    uint64_t stack_size;          /* 所需栈大小 */
    
    uint64_t checksum;            /* 校验和 */
    uint64_t signature[2];        /* 数字签名 */
};                                /* 总计：128 字节 */

/* 驱动标志位 */
#define UMDF_FLAG_AUTOLOAD      0x01    /* 自动加载 */
#define UMDF_FLAG_HOTPLUG       0x02    /* 支持热插拔 */
#define UMDF_FLAG_DMA_CAPABLE   0x04    /* 支持 DMA */
#define UMDF_FLAG_PM_CAPABLE    0x08    /* 支持电源管理 */
#define UMDF_FLAG_SIGNED        0x10    /* 已签名验证 */
#define UMDF_FLAG_DEBUG         0x80    /* 调试模式 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 设备对象结构
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 设备对象 (驱动管理的设备实例)
 */
struct umdf_device {
    uint64_t device_id;           /* 设备唯一 ID */
    uint64_t driver_id;           /* 所属驱动 ID */
    
    char name[64];                /* 设备名称 */
    char path[128];               /* 设备路径 (/dev/xxx) */
    
    uint32_t major;               /* 主设备号 */
    uint32_t minor;               /* 次设备号 */
    
    uint64_t base_addr;           /* MMIO 基地址 */
    uint64_t mmio_size;           /* MMIO 区域大小 */
    uint64_t irq;                 /* 中断号 */
    uint64_t dma_channel;         /* DMA 通道 */
    
    void *private_data;           /* 驱动私有数据 */
    
    uint32_t state;               /* 设备状态 */
    uint32_t ref_count;           /* 引用计数 */
    
    uint64_t created_time;        /* 创建时间 */
    uint64_t last_activity;       /* 最后活动时间 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * IPC 消息结构 (驱动与内核通信)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 驱动请求消息
 */
struct umdf_request {
    uint64_t request_id;          /* 请求 ID */
    uint64_t device_id;           /* 目标设备 */
    
    uint32_t op_code;             /* 操作码 */
    uint32_t flags;               /* 标志位 */
    
    uint64_t buffer_addr;         /* 数据缓冲区地址 */
    uint64_t buffer_size;         /* 缓冲区大小 */
    
    uint64_t offset;              /* 偏移量 */
    uint64_t length;              /* 传输长度 */
    
    uint64_t param[4];            /* 附加参数 */
};                                /* 总计：64 字节 */

/**
 * @brief 驱动响应消息
 */
struct umdf_response {
    uint64_t request_id;          /* 对应的请求 ID */
    int64_t result;               /* 返回结果 (负值=错误) */
    
    uint64_t bytes_transferred;   /* 实际传输字节数 */
    uint64_t latency_ns;          /* 延迟 (纳秒) */
    
    uint64_t param[4];            /* 附加信息 */
};                                /* 总计：48 字节 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 标准操作码定义
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 通用操作 */
#define UMDF_OP_INIT            0x01    /* 初始化设备 */
#define UMDF_OP_CLEANUP         0x02    /* 清理设备 */
#define UMDF_OP_START           0x03    /* 启动设备 */
#define UMDF_OP_STOP            0x04    /* 停止设备 */
#define UMDF_OP_RESET           0x05    /* 重置设备 */

/* I/O 操作 */
#define UMDF_OP_READ            0x10    /* 读取数据 */
#define UMDF_OP_WRITE           0x11    /* 写入数据 */
#define UMDF_OP_IOCTL           0x12    /* 设备控制 */
#define UMDF_OP_MMAP            0x13    /* 内存映射 */

/* 电源管理 */
#define UMDF_OP_PM_SUSPEND      0x20    /* 挂起 */
#define UMDF_OP_PM_RESUME       0x21    /* 恢复 */
#define UMDF_OP_PM_SHUTDOWN     0x22    /* 关机 */

/* 特定设备操作 */
#define UMDF_OP_NET_OPEN        0x30    /* 打开网络设备 */
#define UMDF_OP_NET_SEND        0x31    /* 发送数据包 */
#define UMDF_OP_NET_RECV        0x32    /* 接收数据包 */
#define UMDF_OP_GPU_RENDER      0x40    /* GPU 渲染 */
#define UMDF_OP_AUDIO_PLAY      0x50    /* 播放音频 */
#define UMDF_OP_AUDIO_RECORD    0x51    /* 录音 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 驱动管理器 API
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 驱动生命周期管理 */
int umdf_load_driver(const char *path, uint64_t *driver_id);
int umdf_unload_driver(uint64_t driver_id);
int umdf_start_driver(uint64_t driver_id);
int umdf_stop_driver(uint64_t driver_id);

/* 设备管理 */
int umdf_enumerate_devices(uint64_t driver_id, struct umdf_device *devices, int max_count);
int umdf_open_device(uint64_t device_id, uint64_t *handle);
int umdf_close_device(uint64_t handle);

/* I/O 操作 */
int umdf_read(uint64_t handle, void *buf, size_t count, uint64_t offset);
int umdf_write(uint64_t handle, const void *buf, size_t count, uint64_t offset);
int umdf_ioctl(uint64_t handle, uint32_t code, void *arg);

/* 异步 I/O */
int umdf_submit_request(struct umdf_request *req);
int umdf_poll_response(uint64_t *response_available);
int umdf_get_response(struct umdf_response *resp);

/* 查询接口 */
int umdf_get_driver_info(uint64_t driver_id, struct umdf_driver_info *info);
int umdf_get_device_info(uint64_t device_id, struct umdf_device *info);
int umdf_list_drivers(uint64_t *ids, int max_count);
int umdf_list_devices(uint64_t *ids, int max_count);

/* 底层硬件访问 (需要 capability) */
int umdf_mmio_map(uint64_t device_id, uint64_t addr, size_t size, void **mapped);
int umdf_mmio_unmap(void *mapped);
int umdf_dma_alloc(size_t size, void **dma_buf, uint64_t *dma_addr);
int umdf_dma_free(void *dma_buf, size_t size);
int umdf_register_interrupt(uint64_t irq, void (*handler)(void *), void *data);
int umdf_unregister_interrupt(uint64_t irq);

#endif /* UMDF_H */
