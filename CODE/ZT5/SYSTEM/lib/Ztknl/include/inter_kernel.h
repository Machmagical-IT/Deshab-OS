/*
 * Zratro OS - Inter-Kernel Communication
 *
 * 双内核通信模块 - 通过共享内存和 IPI 实现 Ztknl 与 Azknl 之间的通信
 */

#ifndef INTER_KERNEL_H
#define INTER_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include "smp.h"

/* 前向声明 */
struct interrupt_frame;

/* ========== 协议定义 ========== */

/* 魔数: "ZJAN" (Zratro Adzin Joint) */
#define INTER_KERNEL_MAGIC     0x5A4A414E

/* 协议版本 */
#define INTER_KERNEL_VERSION   1

/* 共享内存基地址（物理地址，双方约定） */
#define INTER_KERNEL_SHM_PHYS  0x1000000   /* 16MB */

/* 共享内存大小 */
#define INTER_KERNEL_SHM_SIZE  0x10000     /* 64KB */

/* IPI 向量使用 smp.h 中定义的 IPI_ZTKNL_TO_AZKNL 和 IPI_AZKNL_TO_ZTKNL */

/* 消息类型 */
enum inter_kernel_msg_type {
    MSG_TYPE_HELLO = 1,          /* 初始化问候 */
    MSG_TYPE_READY = 2,          /* 就绪通知 */
    MSG_TYPE_PING = 3,           /* 心跳检测 */
    MSG_TYPE_PONG = 4,           /* 心跳响应 */
    MSG_TYPE_TASK_REQUEST = 5,   /* 任务请求 */
    MSG_TYPE_TASK_RESPONSE = 6,  /* 任务响应 */
    MSG_TYPE_DATA_SHARE = 7,     /* 数据共享 */
    MSG_TYPE_SYSCALL_PROXY = 8,  /* 系统调用代理 */
    MSG_TYPE_SHUTDOWN = 9,       /* 关闭通知 */
    MSG_TYPE_MAX
};

/* ========== 数据结构 ========== */

/* 共享头部信息 (256 字节) */
struct inter_kernel_header {
    /* 0x000: 魔数和版本 */
    uint32_t magic;              /* 魔数 */
    uint32_t version;            /* 协议版本 */

    /* 0x008: 共享内存信息 */
    uint64_t shm_phys;           /* 共享内存物理地址 */
    uint64_t shm_size;           /* 共享内存大小 */

    /* 0x018: Ztknl 状态 */
    volatile uint32_t ztknl_ready;       /* Ztknl 就绪标志 */
    volatile uint32_t ztknl_cpu_count;   /* Ztknl 使用的 CPU 数量 */
    uint32_t ztknl_apic_id;     /* Ztknl BSP APIC ID */

    /* 0x024: Azknl 状态 */
    volatile uint32_t azknl_ready;       /* Azknl 就绪标志 */
    volatile uint32_t azknl_cpu_count;   /* Azknl 使用的 CPU 数量 */
    uint32_t azknl_apic_id;     /* Azknl BSP APIC ID */

    /* 0x030: 同步原语 */
    volatile uint64_t lock;              /* 原子锁 */
    volatile uint64_t ipi_flag;          /* IPI 待处理标志 */
    volatile uint64_t ipi_data;          /* IPI 数据 */

    /* 0x048: 消息队列状态 */
    volatile uint32_t ztknl_to_azknl_count;  /* Ztknl->AzknL 队列消息数 */
    volatile uint32_t azknl_to_ztknl_count;  /* Azknl->Ztknl 队列消息数 */

    /* 0x050: 保留对齐 */
    uint8_t reserved[0x100 - 0x050];
} __attribute__((packed));

/* 消息结构 (64 字节) */
struct inter_kernel_msg {
    uint64_t id;                 /* 消息 ID */
    uint64_t type;               /* 消息类型 */
    uint64_t timestamp;          /* 时间戳 */
    uint64_t data[5];            /* 消息数据 */
    uint8_t reserved[16];        /* 保留对齐 */
} __attribute__((packed));

/* 消息队列 (4KB) */
#define INTER_KERNEL_QUEUE_SIZE  4096
#define INTER_KERNEL_QUEUE_MSG_COUNT  56

struct inter_kernel_queue {
    volatile uint32_t head;              /* 队列头部索引 */
    volatile uint32_t tail;              /* 队列尾部索引 */
    volatile uint32_t count;             /* 当前消息数 */
    volatile uint32_t capacity;          /* 队列容量 */
    uint8_t reserved[244];               /* 保留对齐 */
    struct inter_kernel_msg messages[INTER_KERNEL_QUEUE_MSG_COUNT];
} __attribute__((packed));

/* 完整共享内存布局 */
struct inter_kernel_shm {
    struct inter_kernel_header header;                  /* 0x0000, 256 字节 */
    struct inter_kernel_queue ztknl_to_azknl;           /* 0x0100, 4KB */
    struct inter_kernel_queue azknl_to_ztknl;           /* 0x1100, 4KB */
    uint8_t data_area[INTER_KERNEL_SHM_SIZE - 0x2100];  /* 0x2100, 剩余空间 */
} __attribute__((packed));

/* ========== 内核角色 ========== */

typedef enum {
    INTER_KERNEL_ROLE_NONE = 0,
    INTER_KERNEL_ROLE_PRIMARY,     /* Ztknl 主内核 */
    INTER_KERNEL_ROLE_SECONDARY    /* Azknl 辅助内核 */
} inter_kernel_role_t;

/* ========== API 接口 ========== */

/**
 * 初始化双内核通信
 * @return 0 成功, -1 失败
 */
int inter_kernel_init(void);

/**
 * 获取当前内核角色
 * @return 角色枚举
 */
inter_kernel_role_t inter_kernel_get_role(void);

/**
 * 检查对方内核是否就绪
 * @return 1 就绪, 0 未就绪
 */
int inter_kernel_is_peer_ready(void);

/**
 * 发送消息到对方内核
 * @param type 消息类型
 * @param data 消息数据
 * @param count 数据项数量
 * @return 0 成功, -1 失败, -2 队列满
 */
int inter_kernel_send_msg(uint64_t type, const uint64_t *data, size_t count);

/**
 * 接收来自对方内核的消息
 * @param msg 消息缓冲区
 * @return 0 成功, -1 失败, -2 队列空
 */
int inter_kernel_recv_msg(struct inter_kernel_msg *msg);

/**
 * 检查是否有待处理消息
 * @return 消息数量
 */
uint32_t inter_kernel_pending_msgs(void);

/**
 * 发送 IPI 通知对方
 */
void inter_kernel_notify_peer(void);

/**
 * IPI 中断处理程序
 * @param frame 中断帧
 */
void inter_kernel_ipi_handler(struct interrupt_frame *frame);

/**
 * 发送关闭通知到对方内核
 * @return 0 成功, -1 失败
 */
int inter_kernel_shutdown_notify(void);

/**
 * 获取共享内存信息
 * @param base 共享内存基地址输出
 * @param size 共享内存大小输出
 */
void inter_kernel_get_shm_info(uint64_t *base, uint64_t *size);

#endif /* INTER_KERNEL_H */