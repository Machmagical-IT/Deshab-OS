#ifndef DISK_H
#define DISK_H

#include <stdint.h>

/* 磁盘类型 */
#define DISK_TYPE_NONE      0
#define DISK_TYPE_ATA       1
#define DISK_TYPE_AHCI      2
#define DISK_TYPE_NVME      3

/* AHCI/ATA 端口 (默认值，可被 PCI BAR 覆盖) */
#define ATA_DATA_DEFAULT        0x1F0
#define ATA_ERROR_DEFAULT       0x1F1
#define ATA_FEATURES_DEFAULT    0x1F1
#define ATA_SECTOR_COUNT_DEFAULT 0x1F2
#define ATA_LBA_LOW_DEFAULT     0x1F3
#define ATA_LBA_MID_DEFAULT     0x1F4
#define ATA_LBA_HIGH_DEFAULT    0x1F5
#define ATA_DRIVE_DEFAULT       0x1F6
#define ATA_STATUS_DEFAULT      0x1F7
#define ATA_COMMAND_DEFAULT     0x1F7

/* IDE 端口地址 (从 PCI BAR 获取) */
extern uint16_t ide_primary_cmd;    /* 主通道命令端口 (BAR0) */
extern uint16_t ide_primary_ctrl;   /* 主通道控制端口 (BAR1) */
extern uint16_t ide_secondary_cmd;  /* 从通道命令端口 (BAR2) */
extern uint16_t ide_secondary_ctrl; /* 从通道控制端口 (BAR3) */
extern int ide_ports_initialized;   /* IDE 端口是否已初始化 */

/* 动态 ATA 端口宏 */
#define ATA_DATA        (ide_primary_cmd + 0)
#define ATA_ERROR       (ide_primary_cmd + 1)
#define ATA_FEATURES    (ide_primary_cmd + 1)
#define ATA_SECTOR_COUNT (ide_primary_cmd + 2)
#define ATA_LBA_LOW     (ide_primary_cmd + 3)
#define ATA_LBA_MID     (ide_primary_cmd + 4)
#define ATA_LBA_HIGH    (ide_primary_cmd + 5)
#define ATA_DRIVE       (ide_primary_cmd + 6)
#define ATA_STATUS      (ide_primary_cmd + 7)
#define ATA_COMMAND     (ide_primary_cmd + 7)

/* ATA 状态位 */
#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRDY 0x40
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

/* ATA 命令 */
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_READ_PIO_EXT 0x24
#define ATA_CMD_IDENTIFY    0xEC

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：Block I/O 调度器
 * 
 * 特性：
 * - 电梯调度算法 (减少寻道时间)
 * - 请求合并 (合并相邻请求)
 * - 优先级队列 (实时/普通/后台)
 * - NCQ 优化 (原生命令队列)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* I/O 请求方向 */
#define IO_DIR_READ     0
#define IO_DIR_WRITE    1

/* I/O 优先级 */
#define IO_PRIO_REALTIME    0   /* 实时优先级 */
#define IO_PRIO_NORMAL      1   /* 普通优先级 */
#define IO_PRIO_BE          2   /* 最佳努力 */
#define IO_PRIO_IDLE        3   /* 空闲时执行 */

/* I/O 请求标志 */
#define IO_FLAG_SYNC        (1 << 0)   /* 同步 I/O */
#define IO_FLAG_META        (1 << 1)   /* 元数据 */
#define IO_FLAG_PRIO        (1 << 2)   /* 高优先级 */
#define IO_FLAG_NOMERGE     (1 << 3)   /* 禁止合并 */

/* 最大请求大小 (扇区数) */
#define IO_MAX_SECTORS      256

/* 最大合并距离 (扇区数) */
#define IO_MERGE_DISTANCE   64

/* I/O 请求结构 */
struct bio_request {
    uint64_t lba;               /* 起始扇区 */
    uint32_t sectors;           /* 扇区数 */
    uint8_t direction;          /* 读/写 */
    uint8_t priority;           /* 优先级 */
    uint8_t flags;              /* 标志 */
    uint8_t merged_count;       /* 合并的请求数 */
    
    void *buffer;               /* 数据缓冲区 */
    void (*callback)(struct bio_request *req, int status);  /* 完成回调 */
    void *private;              /* 私有数据 */
    
    struct bio_request *next;   /* 链表指针 */
    uint64_t submit_time;       /* 提交时间 */
    uint64_t deadline;          /* 截止时间 */
};

/* I/O 调度器统计 */
struct io_sched_stats {
    uint64_t requests_submitted;
    uint64_t requests_merged;
    uint64_t requests_completed;
    uint64_t sectors_read;
    uint64_t sectors_written;
    uint64_t merge_attempts;
    uint64_t merge_successes;
    uint64_t avg_latency_ns;
    uint64_t max_latency_ns;
};

/* 调度器配置 */
struct io_sched_config {
    int elevator_direction;     /* 电梯方向: 0=上升, 1=下降 */
    uint64_t read_expire_ns;    /* 读请求过期时间 */
    uint64_t write_expire_ns;   /* 写请求过期时间 */
    uint32_t max_batch;         /* 最大批处理数 */
    int enable_merge;           /* 启用合并 */
    int enable_sort;            /* 启用排序 */
};

/* 初始化磁盘 */
void disk_init(void);

/* I/O 调度器初始化 */
void io_scheduler_init(void);

/* 提交 I/O 请求 */
int io_submit_request(struct bio_request *req);

/* 提交同步 I/O */
int io_submit_sync(uint64_t lba, uint32_t sectors, void *buffer, int direction);

/* 处理 I/O 队列 */
void io_process_queue(void);

/* 获取调度器统计 */
void io_get_stats(struct io_sched_stats *stats);

/* 读取扇区 */
int disk_read_sector(uint64_t lba, void *buf);

/* 写入扇区 */
int disk_write_sector(uint64_t lba, const void *buf);

/* 获取磁盘大小 (扇区数) */
uint64_t disk_get_size(void);

/* 检查磁盘是否可用 */
int disk_is_available(void);

/* 获取磁盘类型 */
int disk_get_type(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * 多磁盘设备支持
 * 支持同时访问多个U盘/存储设备
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_DISK_DEVICES    8

/* 磁盘设备结构 */
struct disk_device {
    int type;                   /* 磁盘类型 */
    int index;                  /* 设备索引 */
    uint64_t sectors;           /* 扇区数 */
    uint32_t sector_size;       /* 扇区大小 */
    char name[32];              /* 设备名称 */
    char vendor[16];            /* 厂商 */
    char product[32];           /* 产品名 */
    void *private_data;         /* 私有数据 (MSC设备指针等) */
    int initialized;            /* 是否已初始化 */
};

/* 磁盘设备接口 */
struct disk_ops {
    int (*read_sector)(struct disk_device *dev, uint64_t lba, void *buf);
    int (*write_sector)(struct disk_device *dev, uint64_t lba, const void *buf);
};

/* 注册磁盘设备 */
int disk_register_device(struct disk_device *dev, struct disk_ops *ops);

/* 获取磁盘设备 */
struct disk_device *disk_get_device(int index);

/* 获取磁盘设备数量 */
int disk_get_device_count(void);

/* 磁盘设备读写 */
int disk_dev_read_sector(int dev_index, uint64_t lba, void *buf);
int disk_dev_write_sector(int dev_index, uint64_t lba, const void *buf);

/* 初始化所有MSC设备 */
void disk_init_all_msc(void);

#endif
