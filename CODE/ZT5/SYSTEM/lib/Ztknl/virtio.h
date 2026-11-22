#ifndef VIRTIO_H
#define VIRTIO_H

#include "stdint.h"

/* VirtIO PCI 配置 */
#define VIRTIO_PCI_VENDOR_ID        0x1AF4  /* Red Hat */
#define VIRTIO_PCI_DEVICE_ID_MIN    0x1000
#define VIRTIO_PCI_DEVICE_ID_MAX    0x107F

/* VirtIO 设备类型 */
#define VIRTIO_BLK_DEVICE_ID        0x1001

/* VirtIO PCI 配置偏移 */
#define VIRTIO_PCI_HOST_FEATURES    0x00
#define VIRTIO_PCI_GUEST_FEATURES   0x04
#define VIRTIO_PCI_QUEUE_PFN        0x08
#define VIRTIO_PCI_QUEUE_NUM        0x0C
#define VIRTIO_PCI_QUEUE_NOTIFY     0x10
#define VIRTIO_PCI_STATUS           0x12
#define VIRTIO_PCI_ISR              0x13
#define VIRTIO_PCI_CONFIG           0x14

/* VirtIO 状态标志 */
#define VIRTIO_STATUS_RESET         0x00
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_DEVICE_NEED_RESET 0x40
#define VIRTIO_STATUS_FAILED        0x80

/* VirtIO 特性 */
#define VIRTIO_BLK_F_SIZE_BIT       1
#define VIRTIO_BLK_F_SEG_MAX        2
#define VIRTIO_BLK_F_GEOMETRY       4
#define VIRTIO_BLK_F_RO             5
#define VIRTIO_BLK_F_BLK_SIZE       6
#define VIRTIO_BLK_F_BARRIER        0
#define VIRTIO_BLK_F_SCSI           7
#define VIRTIO_BLK_F_FLUSH          9
#define VIRTIO_F_RING_INDIRECT_DESC 28
#define VIRTIO_F_RING_EVENT_IDX     29
#define VIRTIO_F_VERSION_1          32

/* VirtQueue */
#define VIRTIO_RING_SIZE            256

struct virtio_blk_config {
    uint64_t capacity;
    uint32_t size_max;
    uint32_t seg_max;
    uint16_t geometry[4];
    uint32_t blk_size;
    uint8_t physical_block_exp;
    uint8_t alignment_offset;
    uint16_t min_io_size;
    uint32_t opt_io_size;
    uint8_t writeback;
    uint8_t unused0[3];
    uint32_t max_discard_sectors;
    uint32_t max_discard_seg;
    uint32_t discard_sector_alignment;
    uint32_t max_write_zeroes_sectors;
    uint32_t max_write_zeroes_seg;
    uint8_t write_zeroes_may_unmap;
    uint8_t unused1[3];
};

/* VirtIO 请求类型 */
#define VIRTIO_BLK_T_IN             0
#define VIRTIO_BLK_T_OUT            1
#define VIRTIO_BLK_T_FLUSH          4

/* VirtIO 状态 */
#define VIRTIO_BLK_S_OK             0
#define VIRTIO_BLK_S_IOERR          1
#define VIRTIO_BLK_S_UNSUPP         2

/* VirtQueue 结构 */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_RING_SIZE];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VIRTIO_RING_SIZE];
} __attribute__((packed));

struct virtio_virtqueue {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    uint16_t queue_index;
    uint16_t free_head;
    uint16_t avail_idx;
    uint16_t last_used_idx;
};

/* virtio-blk 请求 */
struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

/* 全局 virtio-blk 设备 */
extern int virtio_blk_initialized;
extern struct virtio_virtqueue vblk_vq;
extern uint16_t vblk_io_base;
extern struct virtio_blk_config vblk_config;

/* 函数声明 */
int virtio_blk_init(void);
int virtio_blk_is_available(void);
uint64_t virtio_blk_get_capacity(void);
int virtio_blk_read_sector(uint64_t lba, void *buf);
int virtio_blk_write_sector(uint64_t lba, const void *buf);

#endif /* VIRTIO_H */
