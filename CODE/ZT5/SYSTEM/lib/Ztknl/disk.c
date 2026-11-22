#include "disk.h"
#include "ahci.h"
#include "virtio.h"
#include "console.h"
#include "cpu.h"
#include "string.h"
#include "task.h"
#include "pmm.h"  /* For virt_to_phys */
#include "usb.h"  /* USB MSC support */

#include "heap.h"

/* 串口调试输出 (在 console.c 中定义) */
extern void serial_print(const char *str);

/* IDE 端口地址 (从 PCI BAR 获取，默认使用传统端口) */
uint16_t ide_primary_cmd    = 0x1F0;  /* 主通道命令端口 (BAR0) */
uint16_t ide_primary_ctrl   = 0x3F6;  /* 主通道控制端口 (BAR1) */
uint16_t ide_secondary_cmd  = 0x170;  /* 从通道命令端口 (BAR2) */
uint16_t ide_secondary_ctrl = 0x376;  /* 从通道控制端口 (BAR3) */
int ide_ports_initialized = 0;        /* IDE 端口是否已从 PCI 初始化 */

/* 磁盘信息 */
static uint64_t disk_sectors = 0;
static int disk_initialized = 0;

/* 磁盘类型 */
#define DISK_TYPE_NONE      0
#define DISK_TYPE_ATA       1
#define DISK_TYPE_AHCI      2
#define DISK_TYPE_USB_MSC   3   /* USB Mass Storage */
#define DISK_TYPE_VIRTIO    4   /* VirtIO Block */

static int disk_type = DISK_TYPE_NONE;

/* I/O 端口函数已在 cpu.h 中定义 */

/* Forward declarations */
static int try_merge_requests(struct bio_request *existing, struct bio_request *new_req);

/* 等待磁盘就绪 - 带超时 */
static int ata_wait_ready(void) {
    for (volatile int timeout = 0; timeout < 50000000; timeout++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_STATUS_BSY)) {
            return 0;
        }
    }
    serial_print("[ATA] Timeout waiting for ready\n");
    return -1;
}

/* 等待数据就绪 - 带超时 */
static int ata_wait_data(void) {
    for (volatile int timeout = 0; timeout < 50000000; timeout++) {
        uint8_t status = inb(ATA_STATUS);
        
        if (status & ATA_STATUS_ERR) {
            serial_print("[ATA] Error status detected\n");
            return -1;
        }
        
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ)) {
            return 0;
        }
    }
    serial_print("[ATA] Timeout waiting for data\n");
    return -1;
}

/* 读取扇区 (LBA48 模式) */
int disk_read_sector(uint64_t lba, void *buf) {
    if (!disk_initialized) {
        return -1;
    }

    /* 根据磁盘类型选择驱动 */
    if (disk_type == DISK_TYPE_VIRTIO) {
        return virtio_blk_read_sector(lba, buf);
    }
    
    if (disk_type == DISK_TYPE_USB_MSC) {
        struct msc_device *msc = msc_get_device(0);
        if (msc) {
            return msc_read_sector(msc, lba, buf);
        }
        return -1;
    }
    
    if (disk_type == DISK_TYPE_AHCI) {
        return ahci_read_sector(lba, buf);
    }

    /* ATA 模式 */
    serial_print("[ATA] Reading LBA ");
    char lba_buf[30]; int lba_p = 0;
    uint64_t lba_tmp = lba;
    if (lba_tmp == 0) { lba_buf[lba_p++] = '0'; } else { char t[20]; int tp = 0; while (lba_tmp > 0) { t[tp++] = '0' + (lba_tmp % 10); lba_tmp /= 10; } while (tp > 0) lba_buf[lba_p++] = t[--tp]; }
    lba_buf[lba_p++] = '\n'; lba_buf[lba_p+1] = '\0';
    serial_print(lba_buf);

    /* 等待就绪 */
    if (ata_wait_ready() != 0) {
        serial_print("[ATA] Wait ready timeout\n");
        return -1;
    }

    /* 使用 LBA28 模式 (对于小 LBA) */
    if (lba < 0x0FFFFFFF) {
        /* LBA28 模式 */
        serial_print("[ATA] Using LBA28 mode\n");
        outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));  /* LBA28, 主盘 */
        outb(ATA_SECTOR_COUNT, 1);
        outb(ATA_LBA_LOW, lba & 0xFF);
        outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
        outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
        outb(ATA_COMMAND, ATA_CMD_READ_PIO);  /* 0x20 */
    } else {
        /* LBA48 模式 */
        serial_print("[ATA] Using LBA48 mode\n");
        outb(ATA_DRIVE, 0x40);  /* LBA48, 主盘 */
        outb(ATA_SECTOR_COUNT, 0);           /* 高字节 */
        outb(ATA_SECTOR_COUNT, 1);           /* 低字节 */
        outb(ATA_LBA_LOW, (lba >> 24) & 0xFF);
        outb(ATA_LBA_MID, (lba >> 32) & 0xFF);
        outb(ATA_LBA_HIGH, (lba >> 40) & 0xFF);
        outb(ATA_LBA_LOW, lba & 0xFF);
        outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
        outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
        outb(ATA_COMMAND, ATA_CMD_READ_PIO_EXT);  /* 0x24 */
    }

    serial_print("[ATA] Command sent, waiting for data...\n");
    /* 等待数据就绪 - 带重试 */
    int retries = 3;
    while (retries > 0) {
        if (ata_wait_data() == 0) {
            break;  /* 成功 */
        }
        retries--;
        if (retries > 0) {
            serial_print("[ATA] Retrying read...\n");
            if (lba < 0x0FFFFFFF) {
                outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
                outb(ATA_SECTOR_COUNT, 1);
                outb(ATA_LBA_LOW, lba & 0xFF);
                outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
                outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
                outb(ATA_COMMAND, ATA_CMD_READ_PIO);
            } else {
                outb(ATA_DRIVE, 0x40);
                outb(ATA_SECTOR_COUNT, 0);
                outb(ATA_SECTOR_COUNT, 1);
                outb(ATA_LBA_LOW, (lba >> 24) & 0xFF);
                outb(ATA_LBA_MID, (lba >> 32) & 0xFF);
                outb(ATA_LBA_HIGH, (lba >> 40) & 0xFF);
                outb(ATA_LBA_LOW, lba & 0xFF);
                outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
                outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
                outb(ATA_COMMAND, ATA_CMD_READ_PIO_EXT);
            }
        } else {
            serial_print("[ATA] Read failed after retries\n");
            return -1;
        }
    }

    serial_print("[ATA] Data ready, reading...\n");
    /* 读取数据 */
    uint16_t *data = (uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        data[i] = inw(ATA_DATA);
    }

    serial_print("[ATA] Read complete\n");
    return 0;
}

/* 写入扇区 */
int disk_write_sector(uint64_t lba, const void *buf) {
    if (!disk_initialized) {
        return -1;
    }

    /* 根据磁盘类型选择驱动 */
    if (disk_type == DISK_TYPE_VIRTIO) {
        return virtio_blk_write_sector(lba, buf);
    }
    
    if (disk_type == DISK_TYPE_USB_MSC) {
        struct msc_device *msc = msc_get_device(0);
        if (msc) {
            return msc_write_sector(msc, lba, buf);
        }
        return -1;
    }
    
    if (disk_type == DISK_TYPE_AHCI) {
        return ahci_write_sector(lba, buf);
    }

    /* ATA 模式 - LBA48 写入 */
    serial_print("[ATA] Writing LBA ");
    char lba_buf[30]; int lba_p = 0;
    uint64_t lba_tmp = lba;
    if (lba_tmp == 0) { lba_buf[lba_p++] = '0'; } else { char t[20]; int tp = 0; while (lba_tmp > 0) { t[tp++] = '0' + (lba_tmp % 10); lba_tmp /= 10; } while (tp > 0) lba_buf[lba_p++] = t[--tp]; }
    lba_buf[lba_p++] = '\n'; lba_buf[lba_p+1] = '\0';
    serial_print(lba_buf);

    /* 等待就绪 */
    if (ata_wait_ready() != 0) {
        serial_print("[ATA] Write: Wait ready timeout\n");
        return -1;
    }

    /* LBA48 写入模式 */
    outb(ATA_DRIVE, 0x40);  /* LBA48, 主盘 */

    /* 发送 LBA 和扇区数 */
    outb(ATA_SECTOR_COUNT, 0);           /* 高字节 */
    outb(ATA_SECTOR_COUNT, 1);           /* 低字节: 1 个扇区 */
    outb(ATA_LBA_LOW, (lba >> 24) & 0xFF);
    outb(ATA_LBA_MID, (lba >> 32) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 40) & 0xFF);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);

    /* 发送写入命令 */
    outb(ATA_COMMAND, 0x34);  /* ATA_CMD_WRITE_PIO_EXT */

    serial_print("[ATA] Write command sent, waiting for DRQ...\n");
    /* 等待数据就绪 */
    if (ata_wait_data() != 0) {
        serial_print("[ATA] Write: Wait data timeout\n");
        return -1;
    }

    serial_print("[ATA] Writing data...\n");
    /* 写入数据 */
    const uint16_t *data = (const uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        outw(ATA_DATA, data[i]);
    }

    /* 等待写入完成 */
    if (ata_wait_ready() != 0) {
        serial_print("[ATA] Write: Final wait timeout\n");
        return -1;
    }

    /* 发送 FLUSH CACHE 命令确保数据写入磁盘 */
    outb(ATA_COMMAND, 0xEA);  /* FLUSH CACHE EXT */
    ata_wait_ready();

    serial_print("[ATA] Write complete\n");
    return 0;
}

/* 识别磁盘 */
static int ata_identify(void) {
    outb(ATA_DRIVE, 0x40);  /* LBA 模式, 主盘 */
    
    /* 短暂延迟 */
    for (volatile int i = 0; i < 10000; i++);
    
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    /* 等待响应 */
    if (ata_wait_data() != 0) {
        return -1;
    }

    uint16_t data[256];
    for (int i = 0; i < 256; i++) {
        data[i] = inw(ATA_DATA);
    }

    /* 获取 LBA48 支持和扇区数 */
    if (data[83] & (1 << 10)) {
        /* LBA48 支持 */
        disk_sectors = ((uint64_t)data[100] << 48) |
                       ((uint64_t)data[101] << 32) |
                       ((uint64_t)data[102] << 16) |
                       data[103];
    } else {
        /* LBA28 */
        disk_sectors = ((uint32_t)data[60] << 16) | data[61];
    }

    return 0;
}

void disk_init(void) {
    serial_print("[DISK] Initializing disk...\n");
    /* 首先尝试非USB存储设备 (AHCI/ATA) 以避免干扰引导设备 */
    ahci_init();
    
    if (ahci_is_available()) {
        disk_type = DISK_TYPE_AHCI;
        disk_sectors = ahci_get_disk_size();
        disk_initialized = 1;
        serial_print("[DISK] AHCI disk initialized\n");
        /* 即使 AHCI 成功，也要初始化 USB 以支持 HID 设备（鼠标/键盘） */
        goto usb_init_after_disk;
    }

    serial_print("[DISK] AHCI not available, trying ATA...\n");
    /* AHCI 不可用，尝试传统 ATA (IDE 模式) */
    if (ata_identify() == 0) {
        disk_type = DISK_TYPE_ATA;
        disk_initialized = 1;
        serial_print("[DISK] ATA disk initialized\n");
        /* 即使 ATA 成功，也要初始化 USB 以支持 HID 设备（鼠标/键盘） */
        goto usb_init_after_disk;
    }
    
    serial_print("[DISK] ATA not available, trying VirtIO...\n");
    /* 尝试 VirtIO Block */
    if (virtio_blk_init() == 0) {
        disk_type = DISK_TYPE_VIRTIO;
        disk_sectors = virtio_blk_get_capacity();
        disk_initialized = 1;
        serial_print("[DISK] VirtIO block device initialized\n");
        /* 即使 VirtIO 成功，也要初始化 USB 以支持 HID 设备（鼠标/键盘） */
        goto usb_init_after_disk;
    }

    serial_print("[DISK] VirtIO failed, trying USB...\n");

    /* 初始化 USB 子系统 */
    usb_init();
    usb_scan_devices();
    
    int msc_count = msc_get_device_count();
    if (msc_count > 0) {
        struct msc_device *msc = msc_get_device(0);
        if (msc) {
            disk_type = DISK_TYPE_USB_MSC;
            uint64_t blocks;
            uint32_t block_size;
            msc_get_capacity(msc, &blocks, &block_size);
            disk_sectors = blocks;
            disk_initialized = 1;
            return;
        }
    }

    /* 如果没有找到任何磁盘，输出警告并设置为RAMFS回退模式 */
    serial_print("[DISK] WARNING: No disk found (AHCI/ATA/USB)! Using RAMFS fallback\n");
    disk_type = DISK_TYPE_NONE;
    disk_initialized = 0;
    return;

usb_init_after_disk:
    /* USB 初始化用于 HID 设备（鼠标/键盘），即使磁盘是非USB的 */
    usb_init();
    usb_scan_devices();
    serial_print("[DISK] USB initialized for HID devices\n");
}

uint64_t disk_get_size(void) {
    return disk_sectors;
}

int disk_is_available(void) {
    return disk_initialized;
}

int disk_get_type(void) {
    return disk_type;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：Block I/O 调度器实现
 *
 * 电梯算法 + Deadline 调度器 + 请求合并 + 优先级队列
 * ═══════════════════════════════════════════════════════════════════════════ */

/* I/O 请求队列 */
#define IO_QUEUE_SIZE       64
#define IO_PRIORITY_LEVELS  4

struct io_queue {
    struct bio_request *requests[IO_QUEUE_SIZE];
    uint32_t count;
    uint32_t head;
    uint32_t tail;
    volatile int lock;
};

/* 优先级队列数组 */
static struct io_queue io_queues[IO_PRIORITY_LEVELS];

/* 当前电梯位置 */
static uint64_t elevator_pos = 0;
static int elevator_dir = 0;  /* 0=上升, 1=下降 */

/* 统计 */
static struct io_sched_stats io_stats;

/* 调度器类型: 0=elevator, 1=deadline */
static int io_scheduler_type = 0;

/* 配置 */
static struct io_sched_config io_config = {
    .elevator_direction = 0,
    .read_expire_ns = 500000000ULL,    /* 500ms */
    .write_expire_ns = 5000000000ULL,  /* 5s */
    .max_batch = 8,
    .enable_merge = 1,
    .enable_sort = 1
};

/* 待处理请求缓存 (用于合并) */
static struct bio_request *pending_read = NULL;
static struct bio_request *pending_write = NULL;

/* ═══════════════════════════════════════════════════════════════════════════
 * Deadline I/O 调度器
 *
 * 特性:
 * - 读写分离的排序队列 (按 LBA 排序)
 * - 读写分离的 FIFO 过期队列 (按提交时间排序)
 * - 读请求优先 (防止写饥饿读)
 * - 过期请求立即派发
 * - 批量顺序派发 (减少寻道)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DEADLINE_READ_QUEUE_SIZE   32
#define DEADLINE_WRITE_QUEUE_SIZE  64
#define DEADLINE_READ_EXPIRE_MS    500    /* 读请求过期时间 500ms */
#define DEADLINE_WRITE_EXPIRE_MS   5000   /* 写请求过期时间 5s */
#define DEADLINE_MAX_BATCH         16     /* 最大批量派发数 */
#define DEADLINE_WRITES_STARVED    4      /* 读请求最多连续派发次数后必须处理写 */

/* Deadline 排序队列项 */
struct deadline_entry {
    struct bio_request *req;
    uint64_t lba;              /* 用于排序的 LBA */
    uint64_t submit_tick;      /* 提交时的 tick */
    int in_use;
};

/* Deadline 调度器 */
struct deadline_scheduler {
    /* 读排序队列 (按 LBA 排序的数组) */
    struct deadline_entry read_sort[DEADLINE_READ_QUEUE_SIZE];
    int read_count;

    /* 写排序队列 (按 LBA 排序的数组) */
    struct deadline_entry write_sort[DEADLINE_WRITE_QUEUE_SIZE];
    int write_count;

    /* 读 FIFO 队列 (按提交时间排序) */
    struct deadline_entry read_fifo[DEADLINE_READ_QUEUE_SIZE];
    int read_fifo_head;
    int read_fifo_tail;

    /* 写 FIFO 队列 (按提交时间排序) */
    struct deadline_entry write_fifo[DEADLINE_WRITE_QUEUE_SIZE];
    int write_fifo_head;
    int write_fifo_tail;

    /* 当前调度位置 */
    uint64_t last_sector;      /* 上一次派发的扇区 */
    int last_direction;        /* 上一次方向: 0=读, 1=写 */
    int sequential_count;      /* 连续同方向派发计数 */
    int writes_starved;        /* 写饥饿计数 (连续派发读的次数) */

    /* 统计 */
    uint64_t reads_dispatched;
    uint64_t writes_dispatched;
    uint64_t reads_expired;
    uint64_t writes_expired;
    uint64_t merges;
};

static struct deadline_scheduler deadline;

/* 初始化 Deadline 调度器 */
static void deadline_init(struct deadline_scheduler *ds) {
    memset(ds, 0, sizeof(*ds));
    ds->read_fifo_head = 0;
    ds->read_fifo_tail = 0;
    ds->write_fifo_head = 0;
    ds->write_fifo_tail = 0;
    ds->last_sector = 0;
    ds->last_direction = 0;
    ds->sequential_count = 0;
    ds->writes_starved = 0;
}

/* 在排序数组中按 LBA 插入 (插入排序) */
static int deadline_insert_sorted(struct deadline_entry *array, int *count,
                                   int max_count, struct bio_request *req) {
    if (*count >= max_count) return -1;

    uint64_t lba = req->lba;
    int pos = *count;

    /* 找到插入位置 */
    for (int i = 0; i < *count; i++) {
        if (array[i].lba > lba) {
            pos = i;
            break;
        }
    }

    /* 移动后续元素 */
    for (int i = *count; i > pos; i--) {
        array[i] = array[i - 1];
    }

    /* 插入 */
    array[pos].req = req;
    array[pos].lba = lba;
    array[pos].submit_tick = req->submit_time;
    array[pos].in_use = 1;
    (*count)++;

    return 0;
}

/* 在 FIFO 队列尾部添加 */
static int deadline_fifo_push(struct deadline_entry *fifo, int *tail,
                               int max_count, struct bio_request *req) {
    int t = *tail;
    fifo[t].req = req;
    fifo[t].lba = req->lba;
    fifo[t].submit_tick = req->submit_time;
    fifo[t].in_use = 1;
    *tail = (t + 1) % max_count;
    return 0;
}

/* 从排序数组中移除指定项 */
static void deadline_remove_sorted(struct deadline_entry *array, int *count, int index) {
    if (index < 0 || index >= *count) return;

    /* 移动后续元素覆盖 */
    for (int i = index; i < *count - 1; i++) {
        array[i] = array[i + 1];
    }
    (*count)--;
    memset(&array[*count], 0, sizeof(struct deadline_entry));
}

/* 从 FIFO 队列头部弹出 */
static struct bio_request *deadline_fifo_pop(struct deadline_entry *fifo,
                                              int *head, int max_count) {
    struct bio_request *req = fifo[*head].req;
    fifo[*head].req = NULL;
    fifo[*head].in_use = 0;
    *head = (*head + 1) % max_count;
    return req;
}

/* Deadline: 添加请求 */
static int deadline_add_request(struct deadline_scheduler *ds, struct bio_request *req) {
    req->submit_time = get_tick_count();
    req->deadline = req->submit_time +
        (req->direction == IO_DIR_READ ? DEADLINE_READ_EXPIRE_MS : DEADLINE_WRITE_EXPIRE_MS);

    if (req->direction == IO_DIR_READ) {
        /* 尝试与读排序队列中的请求合并 */
        for (int i = 0; i < ds->read_count; i++) {
            struct bio_request *existing = ds->read_sort[i].req;
            if (existing && try_merge_requests(existing, req)) {
                ds->merges++;
                io_stats.merge_successes++;
                return 0;
            }
        }

        /* 插入读排序队列 */
        if (deadline_insert_sorted(ds->read_sort, &ds->read_count,
                                    DEADLINE_READ_QUEUE_SIZE, req) != 0) {
            return -1;
        }
        /* 插入读 FIFO 队列 */
        deadline_fifo_push(ds->read_fifo, &ds->read_fifo_tail,
                           DEADLINE_READ_QUEUE_SIZE, req);
    } else {
        /* 尝试与写排序队列中的请求合并 */
        for (int i = 0; i < ds->write_count; i++) {
            struct bio_request *existing = ds->write_sort[i].req;
            if (existing && try_merge_requests(existing, req)) {
                ds->merges++;
                io_stats.merge_successes++;
                return 0;
            }
        }

        /* 插入写排序队列 */
        if (deadline_insert_sorted(ds->write_sort, &ds->write_count,
                                    DEADLINE_WRITE_QUEUE_SIZE, req) != 0) {
            return -1;
        }
        /* 插入写 FIFO 队列 */
        deadline_fifo_push(ds->write_fifo, &ds->write_fifo_tail,
                           DEADLINE_WRITE_QUEUE_SIZE, req);
    }

    return 0;
}

/* Deadline: 检查请求是否过期 */
static int deadline_check_expired(struct deadline_scheduler *ds, uint64_t now) {
    /* 检查读 FIFO 头部是否过期 */
    if (ds->read_fifo_head != ds->read_fifo_tail) {
        struct deadline_entry *entry = &ds->read_fifo[ds->read_fifo_head];
        if (entry->req && now >= entry->req->deadline) {
            ds->reads_expired++;
            return 0;  /* 读请求过期 */
        }
    }

    /* 检查写 FIFO 头部是否过期 */
    if (ds->write_fifo_head != ds->write_fifo_tail) {
        struct deadline_entry *entry = &ds->write_fifo[ds->write_fifo_head];
        if (entry->req && now >= entry->req->deadline) {
            ds->writes_expired++;
            return 1;  /* 写请求过期 */
        }
    }

    return -1;  /* 没有过期请求 */
}

/* Deadline: 在排序队列中查找最接近 last_sector 的请求 */
static struct bio_request *deadline_find_next(struct deadline_entry *array,
                                               int count, uint64_t last_sector) {
    if (count == 0) return NULL;

    /* 查找第一个 LBA >= last_sector 的请求 (顺序扫描) */
    int best_idx = -1;
    uint64_t best_dist = (uint64_t)-1;

    for (int i = 0; i < count; i++) {
        if (!array[i].req) continue;
        uint64_t dist;
        if (array[i].lba >= last_sector) {
            dist = array[i].lba - last_sector;
        } else {
            /* 环绕距离 - 优先选择前方的请求 */
            dist = (uint64_t)-1 - last_sector + array[i].lba + 1;
        }
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }

    if (best_idx >= 0) {
        struct bio_request *req = array[best_idx].req;
        /* 从排序队列中移除 */
        deadline_remove_sorted(array, &count, best_idx);
        /* 注意: count 是局部变量，不会更新外部，但这是查找+移除的简化实现 */
        return req;
    }

    return NULL;
}

/* Deadline: 选择下一个要派发的请求 */
static struct bio_request *deadline_pick_next(struct deadline_scheduler *ds) {
    uint64_t now = get_tick_count();

    /* 1. 检查过期请求 - 过期请求优先派发 */
    int expired = deadline_check_expired(ds, now);
    if (expired == 0 && ds->read_count > 0) {
        /* 读请求过期 - 从读 FIFO 头部取 */
        struct bio_request *req = deadline_fifo_pop(ds->read_fifo, &ds->read_fifo_head,
                                                     DEADLINE_READ_QUEUE_SIZE);
        if (req) {
            /* 同时从排序队列中移除 */
            for (int i = 0; i < ds->read_count; i++) {
                if (ds->read_sort[i].req == req) {
                    deadline_remove_sorted(ds->read_sort, &ds->read_count, i);
                    break;
                }
            }
            ds->reads_dispatched++;
            ds->last_sector = req->lba + req->sectors;
            ds->last_direction = 0;
            return req;
        }
    } else if (expired == 1 && ds->write_count > 0) {
        /* 写请求过期 - 从写 FIFO 头部取 */
        struct bio_request *req = deadline_fifo_pop(ds->write_fifo, &ds->write_fifo_head,
                                                     DEADLINE_WRITE_QUEUE_SIZE);
        if (req) {
            for (int i = 0; i < ds->write_count; i++) {
                if (ds->write_sort[i].req == req) {
                    deadline_remove_sorted(ds->write_sort, &ds->write_count, i);
                    break;
                }
            }
            ds->writes_dispatched++;
            ds->last_sector = req->lba + req->sectors;
            ds->last_direction = 1;
            return req;
        }
    }

    /* 2. 没有过期请求，按 LBA 顺序派发 */
    /* 读优先，但写饥饿超过阈值时切换到写 */
    if (ds->read_count > 0 && ds->writes_starved < DEADLINE_WRITES_STARVED) {
        /* 选择最接近 last_sector 的读请求 */
        int best_idx = -1;
        uint64_t best_dist = (uint64_t)-1;
        for (int i = 0; i < ds->read_count; i++) {
            if (!ds->read_sort[i].req) continue;
            uint64_t dist;
            if (ds->read_sort[i].lba >= ds->last_sector) {
                dist = ds->read_sort[i].lba - ds->last_sector;
            } else {
                dist = (uint64_t)-1 - ds->last_sector + ds->read_sort[i].lba + 1;
            }
            if (dist < best_dist) {
                best_dist = dist;
                best_idx = i;
            }
        }

        if (best_idx >= 0) {
            struct bio_request *req = ds->read_sort[best_idx].req;
            /* 从 FIFO 中也移除 */
            for (int i = 0; i < DEADLINE_READ_QUEUE_SIZE; i++) {
                if (ds->read_fifo[i].req == req) {
                    ds->read_fifo[i].req = NULL;
                    ds->read_fifo[i].in_use = 0;
                    break;
                }
            }
            deadline_remove_sorted(ds->read_sort, &ds->read_count, best_idx);
            ds->reads_dispatched++;
            ds->last_sector = req->lba + req->sectors;
            ds->last_direction = 0;
            ds->writes_starved++;
            return req;
        }
    }

    /* 3. 派发写请求 */
    if (ds->write_count > 0) {
        int best_idx = -1;
        uint64_t best_dist = (uint64_t)-1;
        for (int i = 0; i < ds->write_count; i++) {
            if (!ds->write_sort[i].req) continue;
            uint64_t dist;
            if (ds->write_sort[i].lba >= ds->last_sector) {
                dist = ds->write_sort[i].lba - ds->last_sector;
            } else {
                dist = (uint64_t)-1 - ds->last_sector + ds->write_sort[i].lba + 1;
            }
            if (dist < best_dist) {
                best_dist = dist;
                best_idx = i;
            }
        }

        if (best_idx >= 0) {
            struct bio_request *req = ds->write_sort[best_idx].req;
            for (int i = 0; i < DEADLINE_WRITE_QUEUE_SIZE; i++) {
                if (ds->write_fifo[i].req == req) {
                    ds->write_fifo[i].req = NULL;
                    ds->write_fifo[i].in_use = 0;
                    break;
                }
            }
            deadline_remove_sorted(ds->write_sort, &ds->write_count, best_idx);
            ds->writes_dispatched++;
            ds->last_sector = req->lba + req->sectors;
            ds->last_direction = 1;
            ds->writes_starved = 0;
            return req;
        }
    }

    return NULL;
}

/* 设置调度器类型 */
void io_set_scheduler(int type) {
    io_scheduler_type = type;
    if (type == 1) {
        deadline_init(&deadline);
    }
}

/* 获取调度器类型 */
int io_get_scheduler(void) {
    return io_scheduler_type;
}

/* 初始化 I/O 调度器 */
void io_scheduler_init(void) {
    for (int i = 0; i < IO_PRIORITY_LEVELS; i++) {
        io_queues[i].count = 0;
        io_queues[i].head = 0;
        io_queues[i].tail = 0;
        io_queues[i].lock = 0;
    }
    
    memset(&io_stats, 0, sizeof(io_stats));
    elevator_pos = 0;
    elevator_dir = 0;
    pending_read = NULL;
    pending_write = NULL;
    
    // println("[IO-SCHED] Block I/O scheduler initialized");
}

/* 队列操作 */
static int io_queue_is_full(struct io_queue *q) {
    return q->count >= IO_QUEUE_SIZE;
}

static int io_queue_is_empty(struct io_queue *q) {
    return q->count == 0;
}

static void io_queue_push(struct io_queue *q, struct bio_request *req) {
    if (io_queue_is_full(q)) return;
    q->requests[q->tail] = req;
    q->tail = (q->tail + 1) % IO_QUEUE_SIZE;
    q->count++;
}

static struct bio_request *io_queue_pop(struct io_queue *q) {
    if (io_queue_is_empty(q)) return NULL;
    struct bio_request *req = q->requests[q->head];
    q->head = (q->head + 1) % IO_QUEUE_SIZE;
    q->count--;
    return req;
}

/* 尝试合并请求 */
static int try_merge_requests(struct bio_request *existing, struct bio_request *new_req) {
    if (!io_config.enable_merge) return 0;
    if (new_req->flags & IO_FLAG_NOMERGE) return 0;
    if (existing->direction != new_req->direction) return 0;
    
    /* 检查是否相邻 */
    uint64_t existing_end = existing->lba + existing->sectors;
    
    /* 新请求紧跟在现有请求后面 */
    if (new_req->lba == existing_end && 
        existing->sectors + new_req->sectors <= IO_MAX_SECTORS) {
        /* 合并到现有请求 */
        existing->sectors += new_req->sectors;
        existing->merged_count++;
        
        /* 如果有回调，链接起来 (简化：只保留最后一个) */
        if (new_req->callback) {
            existing->callback = new_req->callback;
            existing->private = new_req->private;
        }
        
        io_stats.merge_successes++;
        return 1;
    }
    
    /* 新请求在现有请求前面 */
    if (new_req->lba + new_req->sectors == existing->lba &&
        existing->sectors + new_req->sectors <= IO_MAX_SECTORS) {
        /* 需要移动缓冲区数据 - 简化实现暂不支持 */
        return 0;
    }
    
    /* 距离足够近，可以合并 */
    if (new_req->lba >= existing_end && 
        new_req->lba - existing_end <= IO_MERGE_DISTANCE &&
        existing->sectors + (new_req->lba - existing_end) + new_req->sectors <= IO_MAX_SECTORS) {
        /* 扩展现有请求 (包含中间空隙) */
        existing->sectors = (new_req->lba + new_req->sectors) - existing->lba;
        existing->merged_count++;
        io_stats.merge_successes++;
        return 1;
    }
    
    return 0;
}

/* 在队列中查找可合并的请求 */
static struct bio_request *find_mergeable_request(struct bio_request *req) {
    struct io_queue *q = &io_queues[req->priority];
    
    for (uint32_t i = 0; i < q->count; i++) {
        uint32_t idx = (q->head + i) % IO_QUEUE_SIZE;
        struct bio_request *existing = q->requests[idx];
        
        if (try_merge_requests(existing, req)) {
            return existing;
        }
    }
    
    return NULL;
}

/* 按电梯算法排序插入 */
static void io_queue_insert_sorted(struct io_queue *q, struct bio_request *req) {
    if (!io_config.enable_sort || io_queue_is_full(q)) {
        io_queue_push(q, req);
        return;
    }
    
    /* 查找插入位置 (按 LBA 顺序) */
    uint32_t insert_pos = q->tail;
    
    if (elevator_dir == 0) {  /* 上升 */
        for (uint32_t i = 0; i < q->count; i++) {
            uint32_t idx = (q->head + i) % IO_QUEUE_SIZE;
            if (q->requests[idx]->lba > req->lba) {
                insert_pos = idx;
                break;
            }
        }
    } else {  /* 下降 */
        for (uint32_t i = 0; i < q->count; i++) {
            uint32_t idx = (q->head + i) % IO_QUEUE_SIZE;
            if (q->requests[idx]->lba < req->lba) {
                insert_pos = idx;
                break;
            }
        }
    }
    
    /* 移动元素 */
    if (insert_pos != q->tail) {
        uint32_t cur = q->tail;
        while (cur != insert_pos) {
            uint32_t prev = (cur - 1 + IO_QUEUE_SIZE) % IO_QUEUE_SIZE;
            q->requests[cur] = q->requests[prev];
            cur = prev;
        }
        q->requests[insert_pos] = req;
        q->count++;
    } else {
        io_queue_push(q, req);
    }
}

/* 提交 I/O 请求 */
int io_submit_request(struct bio_request *req) {
    if (req == NULL || !disk_initialized) {
        return -1;
    }
    
    io_stats.requests_submitted++;
    req->submit_time = get_tick_count();
    req->merged_count = 1;
    
    /* 获取对应优先级的队列 */
    int prio = req->priority;
    if (prio < 0) prio = IO_PRIO_NORMAL;
    if (prio >= IO_PRIORITY_LEVELS) prio = IO_PRIO_IDLE;
    
    struct io_queue *q = &io_queues[prio];
    
    /* 获取锁 */
    while (__sync_lock_test_and_set(&q->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 尝试合并 */
    io_stats.merge_attempts++;
    if (find_mergeable_request(req)) {
        /* 合并成功，不需要添加新请求 */
        io_stats.requests_merged++;
        __sync_lock_release(&q->lock);
        return 0;
    }
    
    /* 检查队列是否已满 */
    if (io_queue_is_full(q)) {
        __sync_lock_release(&q->lock);
        return -1;  /* 队列满 */
    }
    
    /* 插入到队列 */
    io_queue_insert_sorted(q, req);
    
    __sync_lock_release(&q->lock);
    
    return 0;
}

/* 选择下一个请求 (电梯算法) */
static struct bio_request *io_pick_next(void) {
    struct bio_request *best = NULL;
    uint64_t best_distance = (uint64_t)-1;
    
    /* 从高到低优先级查找 */
    for (int p = 0; p < IO_PRIORITY_LEVELS; p++) {
        struct io_queue *q = &io_queues[p];
        
        if (io_queue_is_empty(q)) continue;
        
        /* 查找最近的请求 */
        for (uint32_t i = 0; i < q->count; i++) {
            uint32_t idx = (q->head + i) % IO_QUEUE_SIZE;
            struct bio_request *req = q->requests[idx];
            
            /* 检查截止时间 */
            if (req->deadline != 0 && get_tick_count() > req->deadline) {
                /* 过期请求，立即返回 */
                return io_queue_pop(q);
            }
            
            /* 计算距离 */
            uint64_t distance;
            if (elevator_dir == 0) {  /* 上升 */
                if (req->lba >= elevator_pos) {
                    distance = req->lba - elevator_pos;
                } else {
                    distance = (uint64_t)-1 - elevator_pos + req->lba;
                }
            } else {  /* 下降 */
                if (req->lba <= elevator_pos) {
                    distance = elevator_pos - req->lba;
                } else {
                    distance = elevator_pos + ((uint64_t)-1 - req->lba);
                }
            }
            
            if (distance < best_distance) {
                best = req;
                best_distance = distance;
            }
        }
    }
    
    /* 更新电梯方向 */
    if (best != NULL) {
        if (best->lba > elevator_pos) {
            elevator_dir = 0;  /* 上升 */
        } else if (best->lba < elevator_pos) {
            elevator_dir = 1;  /* 下降 */
        }
        elevator_pos = best->lba + best->sectors;
    }
    
    return best;
}

/* 执行单个请求 */
static int io_execute_request(struct bio_request *req) {
    int ret;
    
    if (req->direction == IO_DIR_READ) {
        /* 批量读取 */
        for (uint32_t i = 0; i < req->sectors; i++) {
            ret = disk_read_sector(req->lba + i, 
                                   (uint8_t *)req->buffer + i * 512);
            if (ret != 0) break;
        }
        io_stats.sectors_read += req->sectors;
    } else {
        /* 批量写入 */
        for (uint32_t i = 0; i < req->sectors; i++) {
            ret = disk_write_sector(req->lba + i,
                                    (const uint8_t *)req->buffer + i * 512);
            if (ret != 0) break;
        }
        io_stats.sectors_written += req->sectors;
    }
    
    /* 更新延迟统计 */
    uint64_t latency = get_tick_count() - req->submit_time;
    io_stats.avg_latency_ns = (io_stats.avg_latency_ns * 7 + latency) / 8;
    if (latency > io_stats.max_latency_ns) {
        io_stats.max_latency_ns = latency;
    }
    
    io_stats.requests_completed++;
    
    /* 调用完成回调 */
    if (req->callback) {
        req->callback(req, ret);
    }
    
    return ret;
}

/* 处理 I/O 队列 */
void io_process_queue(void) {
    uint32_t batch_count = 0;
    
    while (batch_count < io_config.max_batch) {
        struct bio_request *req = io_pick_next();
        if (req == NULL) break;
        
        /* 从队列移除 */
        int prio = req->priority;
        struct io_queue *q = &io_queues[prio];
        
        while (__sync_lock_test_and_set(&q->lock, 1)) {
            asm volatile("pause");
        }
        
        /* 查找并移除请求 */
        for (uint32_t i = 0; i < q->count; i++) {
            uint32_t idx = (q->head + i) % IO_QUEUE_SIZE;
            if (q->requests[idx] == req) {
                /* 移动后续元素 */
                for (uint32_t j = i; j < q->count - 1; j++) {
                    uint32_t cur = (q->head + j) % IO_QUEUE_SIZE;
                    uint32_t next = (q->head + j + 1) % IO_QUEUE_SIZE;
                    q->requests[cur] = q->requests[next];
                }
                q->tail = (q->tail - 1 + IO_QUEUE_SIZE) % IO_QUEUE_SIZE;
                q->count--;
                break;
            }
        }
        
        __sync_lock_release(&q->lock);
        
        /* 执行请求 */
        io_execute_request(req);
        batch_count++;
    }
}

/* 提交同步 I/O */
int io_submit_sync(uint64_t lba, uint32_t sectors, void *buffer, int direction) {
    if (!disk_initialized) return -1;
    
    struct bio_request req = {
        .lba = lba,
        .sectors = sectors,
        .direction = (uint8_t)direction,
        .priority = IO_PRIO_NORMAL,
        .flags = IO_FLAG_SYNC,
        .buffer = buffer,
        .callback = NULL,
        .private = NULL,
        .submit_time = get_tick_count(),
        .deadline = get_tick_count() + io_config.read_expire_ns
    };
    
    /* 同步请求直接执行 */
    return io_execute_request(&req);
}

/* 获取调度器统计 */
void io_get_stats(struct io_sched_stats *stats) {
    if (stats != NULL) {
        *stats = io_stats;
    }
}

/* ========== 驱动模型高级增强功能 ========== */

/* === 设备模型框架 === */

struct device {
    char name[32];
    uint32_t device_id;
    uint32_t vendor_id;
    uint32_t class_code;
    uint64_t resources[6];
    uint32_t irq;
    int state;
    struct device *parent;
    struct device *next;
    void *driver_data;
};

struct device_driver {
    char name[32];
    uint32_t supported_ids[16];
    int (*probe)(struct device *dev);
    void (*remove)(struct device *dev);
    int (*suspend)(struct device *dev);
    int (*resume)(struct device *dev);
};

static struct device *device_list = NULL;
static struct device_driver *driver_list = NULL;
static int device_next_id = 1;

/* 注册设备 */
int device_register(struct device *dev) {
    if (dev == NULL) return -1;
    
    dev->device_id = device_next_id++;
    dev->next = device_list;
    device_list = dev;
    
    return dev->device_id;
}

/* 注销设备 */
int device_unregister(struct device *dev) {
    if (dev == NULL) return -1;
    
    struct device **pp = &device_list;
    while (*pp != NULL) {
        if (*pp == dev) {
            *pp = dev->next;
            return 0;
        }
        pp = &(*pp)->next;
    }
    
    return -1;
}

/* 注册驱动 */
int driver_register(struct device_driver *drv) {
    if (drv == NULL) return -1;
    
    drv->name[31] = '\0';
    
    /* 尝试匹配已有设备 */
    struct device *dev = device_list;
    while (dev != NULL) {
        for (int i = 0; i < 16; i++) {
            if (drv->supported_ids[i] == dev->vendor_id << 16 | dev->device_id) {
                if (drv->probe) {
                    drv->probe(dev);
                    dev->driver_data = drv;
                }
                break;
            }
        }
        dev = dev->next;
    }
    
    return 0;
}

/* === 电源管理集成 === */

#define PM_STATE_RUNNING    0
#define PM_STATE_SUSPENDED  1
#define PM_STATE_OFF        2

struct pm_ops {
    int (*suspend)(void *data);
    int (*resume)(void *data);
    int (*shutdown)(void *data);
};

static struct pm_ops *pm_callbacks[32];
static int pm_callback_count = 0;

/* 注册电源管理回调 */
int pm_register_callback(struct pm_ops *ops) {
    if (ops == NULL || pm_callback_count >= 32) return -1;
    pm_callbacks[pm_callback_count++] = ops;
    return 0;
}

/* 挂起系统 */
int pm_suspend(void) {
    // println("[PM] Suspending system...");
    
    for (int i = 0; i < pm_callback_count; i++) {
        if (pm_callbacks[i]->suspend) {
            pm_callbacks[i]->suspend(NULL);
        }
    }
    
    return 0;
}

/* 恢复系统 */
int pm_resume(void) {
    // println("[PM] Resuming system...");
    
    for (int i = pm_callback_count - 1; i >= 0; i--) {
        if (pm_callbacks[i]->resume) {
            pm_callbacks[i]->resume(NULL);
        }
    }
    
    return 0;
}

/* === DMA 缓冲区管理 === */

struct dma_buffer {
    uint64_t phys_addr;
    void *virt_addr;
    size_t size;
    int direction;
    int in_use;
};

#define DMA_BUF_MAX    16
static struct dma_buffer dma_buffers[DMA_BUF_MAX];

/* 分配 DMA 缓冲区 */
void *dma_alloc_buffer(size_t size, uint64_t *phys_addr) {
    if (size == 0) return NULL;
    
    for (int i = 0; i < DMA_BUF_MAX; i++) {
        if (!dma_buffers[i].in_use) {
            dma_buffers[i].virt_addr = kmalloc(size);
            if (dma_buffers[i].virt_addr == NULL) return NULL;
            
            dma_buffers[i].size = size;
            dma_buffers[i].in_use = 1;
            
            /* 关键修复：Higher Half内核中，kmalloc返回虚拟地址，必须转换为物理地址 */
            dma_buffers[i].phys_addr = virt_to_phys(dma_buffers[i].virt_addr);
            *phys_addr = dma_buffers[i].phys_addr;
            
            return dma_buffers[i].virt_addr;
        }
    }
    
    return NULL;
}

/* 释放 DMA 缓冲区 */
void dma_free_buffer(void *virt_addr) {
    if (virt_addr == NULL) return;
    
    for (int i = 0; i < DMA_BUF_MAX; i++) {
        if (dma_buffers[i].virt_addr == virt_addr && dma_buffers[i].in_use) {
            kfree(virt_addr);
            dma_buffers[i].in_use = 0;
            return;
        }
    }
}

/* 同步 DMA 缓冲区 */
void dma_sync(void *virt_addr, int direction) {
    /* 简化：内存屏障 */
    asm volatile("" ::: "memory");
}

/* === 中断请求管理 === */

#define IRQ_MAX         256
#define IRQ_NAME_LEN    32

struct irq_handler {
    char name[IRQ_NAME_LEN];
    int (*handler)(int irq, void *data);
    void *data;
    int flags;
    int enabled;
};

static struct irq_handler irq_handlers[IRQ_MAX];

/* 注册中断处理程序 */
int request_irq(int irq, int (*handler)(int, void*), const char *name, void *data) {
    if (irq < 0 || irq >= IRQ_MAX || handler == NULL) return -1;
    
    irq_handlers[irq].handler = handler;
    irq_handlers[irq].data = data;
    irq_handlers[irq].enabled = 1;
    
    if (name != NULL) {
        strncpy(irq_handlers[irq].name, name, IRQ_NAME_LEN - 1);
    }
    
    return 0;
}

/* 释放中断处理程序 */
void free_irq(int irq) {
    if (irq >= 0 && irq < IRQ_MAX) {
        irq_handlers[irq].handler = NULL;
        irq_handlers[irq].data = NULL;
        irq_handlers[irq].enabled = 0;
    }
}

/* 处理中断 */
int handle_irq(int irq) {
    if (irq < 0 || irq >= IRQ_MAX) return -1;
    if (!irq_handlers[irq].enabled || irq_handlers[irq].handler == NULL) return -1;
    
    return irq_handlers[irq].handler(irq, irq_handlers[irq].data);
}

/* === 设备诊断 === */

/* 打印设备列表 */
void device_print_list(void) {
    // println("=== Device List ===");
    
    struct device *dev = device_list;
    while (dev != NULL) {
        // print("Device: ");
        // println(dev->name);
        // print("  ID: ");
        print_hex(dev->device_id);
        // print(" Vendor: ");
        print_hex(dev->vendor_id);
        // println("");
        
        dev = dev->next;
    }
}

/* 设备健康检查 */
int device_health_check(struct device *dev) {
    if (dev == NULL) return -1;
    
    /* 简化：检查设备状态 */
    return dev->state == 0 ? 0 : -1;
}

/* 设备重置 */
int device_reset(struct device *dev) {
    if (dev == NULL) return -1;
    
    // println("[DEVICE] Resetting device...");
    dev->state = 0;
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 多磁盘设备支持
 * 支持同时访问多个U盘/存储设备
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 磁盘设备列表 */
static struct disk_device disk_devices[MAX_DISK_DEVICES];
static struct disk_ops disk_ops_list[MAX_DISK_DEVICES];
static int disk_device_count = 0;

/* USB MSC 磁盘操作 */
static int msc_disk_read_sector(struct disk_device *dev, uint64_t lba, void *buf) {
    if (dev == NULL || dev->private_data == NULL) return -1;
    struct msc_device *msc = (struct msc_device *)dev->private_data;
    return msc_read_sector(msc, lba, buf);
}

static int msc_disk_write_sector(struct disk_device *dev, uint64_t lba, const void *buf) {
    if (dev == NULL || dev->private_data == NULL) return -1;
    struct msc_device *msc = (struct msc_device *)dev->private_data;
    return msc_write_sector(msc, lba, buf);
}

/* 注册磁盘设备 */
int disk_register_device(struct disk_device *dev, struct disk_ops *ops) {
    if (disk_device_count >= MAX_DISK_DEVICES) {
        // println("[DISK] Too many disk devices");
        return -1;
    }
    
    int index = disk_device_count;
    
    memcpy(&disk_devices[index], dev, sizeof(struct disk_device));
    disk_devices[index].index = index;
    
    if (ops) {
        memcpy(&disk_ops_list[index], ops, sizeof(struct disk_ops));
    }
    
    disk_device_count++;
    
    // print("[DISK] Registered disk device ");
    print_dec(index);
    // print(": ");
    // print(dev->name);
    // print(" (");
    print_dec(dev->sectors * dev->sector_size / (1024 * 1024));
    // println(" MB)");
    
    return index;
}

/* 获取磁盘设备 */
struct disk_device *disk_get_device(int index) {
    if (index < 0 || index >= disk_device_count) {
        return NULL;
    }
    return &disk_devices[index];
}

/* 获取磁盘设备数量 */
int disk_get_device_count(void) {
    return disk_device_count;
}

/* 磁盘设备读扇区 */
int disk_dev_read_sector(int dev_index, uint64_t lba, void *buf) {
    if (dev_index < 0 || dev_index >= disk_device_count) {
        return -1;
    }
    
    struct disk_device *dev = &disk_devices[dev_index];
    struct disk_ops *ops = &disk_ops_list[dev_index];
    
    /* 优先使用设备特定操作 */
    if (dev->type == DISK_TYPE_USB_MSC && ops->read_sector) {
        return ops->read_sector(dev, lba, buf);
    }
    
    /* 回退到全局磁盘接口 */
    return disk_read_sector(lba, buf);
}

/* 磁盘设备写扇区 */
int disk_dev_write_sector(int dev_index, uint64_t lba, const void *buf) {
    if (dev_index < 0 || dev_index >= disk_device_count) {
        return -1;
    }
    
    struct disk_device *dev = &disk_devices[dev_index];
    struct disk_ops *ops = &disk_ops_list[dev_index];
    
    /* 优先使用设备特定操作 */
    if (dev->type == DISK_TYPE_USB_MSC && ops->write_sector) {
        return ops->write_sector(dev, lba, buf);
    }
    
    /* 回退到全局磁盘接口 */
    return disk_write_sector(lba, buf);
}

/* 初始化所有MSC设备为磁盘设备 */
void disk_init_all_msc(void) {
    int msc_count = msc_get_device_count();
    
    // print("[DISK] Found ");
    print_dec(msc_count);
    // println(" MSC device(s)");
    
    for (int i = 0; i < msc_count; i++) {
        struct msc_device *msc = msc_get_device(i);
        if (msc == NULL) continue;
        
        struct disk_device dev = {0};
        struct disk_ops ops = {0};
        
        dev.type = DISK_TYPE_USB_MSC;
        dev.sectors = msc->block_count;
        dev.sector_size = msc->block_size;
        dev.private_data = msc;
        dev.initialized = 1;
        
        /* 生成设备名称: usb0, usb1, ... */
        dev.name[0] = 'u';
        dev.name[1] = 's';
        dev.name[2] = 'b';
        dev.name[3] = '0' + i;
        dev.name[4] = '\0';
        
        /* 复制厂商和产品信息 */
        memcpy(dev.vendor, msc->vendor, sizeof(dev.vendor));
        memcpy(dev.product, msc->product, sizeof(dev.product));
        
        /* 设置操作 */
        ops.read_sector = msc_disk_read_sector;
        ops.write_sector = msc_disk_write_sector;
        
        /* 注册设备 */
        disk_register_device(&dev, &ops);
    }
    
    // print("[DISK] Total disk devices: ");
    print_dec(disk_device_count);
    // println("");
}
