#include "virtio.h"
#include "console.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"

/* 外部函数声明 */
/* virt_to_phys is defined in pmm.h as static inline */

/* 直接计算物理地址（用于 BSS 段静态变量）
 * 在 x86_64 的 identity mapping 中，低地址的虚拟地址等于物理地址
 */
static inline uint64_t virt_to_phys_direct(void *virt) {
    uint64_t v = (uint64_t)virt;

    /* 如果虚拟地址在低 4GB 范围内，假设 identity mapping */
    if (v < 0x100000000ULL) {
        return v;
    }

    /* 否则使用标准的 virt_to_phys */
    return virt_to_phys(virt);
}

/* 串口调试输出 */
extern void serial_print(const char *str);

/* PCI 配置端口 */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* PCI 配置偏移 */
#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_BAR0         0x10
#define PCI_CLASS_CODE   0x08

/* I/O 端口访问函数已在 cpu.h 中定义 (通过 vmm.h 包含) */

/* PCI 配置访问 */
static uint32_t pci_read_config(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t addr = (1U << 31) | ((uint32_t)bus << 16) | 
                    ((uint32_t)device << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

/* 全局变量 */
int virtio_blk_initialized = 0;
struct virtio_virtqueue vblk_vq;
uint16_t vblk_io_base = 0;
struct virtio_blk_config vblk_config;


/* 查找 virtio-blk PCI 设备 */
static int virtio_blk_find_device(void) {
    serial_print("[VIRTIO] Scanning PCI for virtio-blk device...\n");
    
    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_config(bus, slot, func, PCI_VENDOR_ID);
                
                /* 检查设备是否存在 */
                uint16_t vendor_id = vendor_device & 0xFFFF;
                if (vendor_id == 0xFFFF) {
                    continue;
                }
                
                uint16_t device_id = (vendor_device >> 16) & 0xFFFF;
                
                /* 查找 virtio-blk 设备 */
                if (vendor_id == VIRTIO_PCI_VENDOR_ID && 
                    device_id == VIRTIO_BLK_DEVICE_ID) {
                    
                    serial_print("[VIRTIO] Found virtio-blk device\n");
                    
                    /* 获取 BAR0 (I/O 基地址) */
                    uint32_t bar0 = pci_read_config(bus, slot, func, PCI_BAR0);
                    if (bar0 & 0x1) {
                        /* I/O 空间 */
                        vblk_io_base = bar0 & 0xFFFC;
                        
                        serial_print("[VIRTIO] I/O base: 0x");
                        char buf[20]; int bp = 0;
                        buf[bp++] = '0'; buf[bp++] = 'x';
                        for (int i = 12; i >= 0; i -= 4) buf[bp++] = "0123456789ABCDEF"[(vblk_io_base >> i) & 0xF];
                        buf[bp] = '\n'; buf[bp+1] = '\0';
                        serial_print(buf);
                        return 1;
                    }
                }
            }
        }
    }
    
    serial_print("[VIRTIO] No virtio-blk device found\n");
    return 0;
}

/* 初始化 virtio-blk 设备 */
int virtio_blk_init(void) {
    /* 查找设备 */
    if (!virtio_blk_find_device()) {
        return -1;
    }
    
    serial_print("[VIRTIO] Initializing virtio-blk device...\n");
    
    /* 复位设备 */
    outb(vblk_io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_RESET);
    
    /* 确认设备 */
    outb(vblk_io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    outb(vblk_io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_DRIVER);
    
    /* 协商特性 */
    uint32_t host_features = inl(vblk_io_base + VIRTIO_PCI_HOST_FEATURES);
    uint32_t guest_features = 0;
    guest_features |= (1 << VIRTIO_F_RING_INDIRECT_DESC);
    guest_features |= (1 << VIRTIO_BLK_F_BLK_SIZE);
    outl(vblk_io_base + VIRTIO_PCI_GUEST_FEATURES, guest_features);
    
    serial_print("[VIRTIO] Host features: 0x");
    char buf[20]; buf[0] = '0'; buf[1] = 'x';
    for (int i = 28; i >= 0; i -= 4) buf[2 + (7 - i/4)] = "0123456789ABCDEF"[(host_features >> i) & 0xF];
    buf[10] = '\n'; buf[11] = '\0';
    serial_print(buf);
    
    /* 分配 VirtQueue 内存 (对齐到页)
     * VirtIO 规范要求:
     *   desc: VIRTIO_RING_SIZE * sizeof(virtq_desc) = 256*16 = 4096 bytes
     *   avail: 2+2+256*2+2 = 514 bytes (padding to align used ring)
     *   used: aligned to page boundary, 2+2+256*8 = 2052 bytes
     * 使用 3 页确保足够空间并对齐
     * 注意: VirtIO 传统 PCI 设备只支持 32 位地址，需要低地址内存 */
    uint64_t vq_phys = pmm_alloc_pages(3);
    if (!vq_phys) {
        serial_print("[VIRTIO] Failed to allocate VirtQueue memory\n");
        return -1;
    }
    if (vq_phys >= 0x100000000ULL) {
        serial_print("[VIRTIO] VirtQueue above 4GB!\n");
        pmm_free_page(vq_phys);
        return -1;
    }

    /* 设置队列大小 */
    outw(vblk_io_base + VIRTIO_PCI_QUEUE_NUM, VIRTIO_RING_SIZE);

    /* 设置队列地址 */
    outl(vblk_io_base + VIRTIO_PCI_QUEUE_PFN, vq_phys >> 12);

    /* 映射 VirtQueue 到内核空间 */
    void *vq_virt = phys_to_virt(vq_phys);
    memset(vq_virt, 0, 3 * 4096);

    vblk_vq.desc = (struct virtq_desc *)vq_virt;
    /* avail ring follows desc table */
    size_t avail_offset = VIRTIO_RING_SIZE * sizeof(struct virtq_desc);
    vblk_vq.avail = (struct virtq_avail *)((uint8_t *)vq_virt + avail_offset);
    /* used ring must be page-aligned per VirtIO spec */
    size_t used_offset = (avail_offset + sizeof(struct virtq_avail) + 4095) & ~4095ULL;
    vblk_vq.used = (struct virtq_used *)((uint8_t *)vq_virt + used_offset);
    
    /* VirtQueue already zeroed by the memset above */
    /* 初始化描述符链表 */
    for (uint16_t i = 0; i < VIRTIO_RING_SIZE - 1; i++) {
        vblk_vq.desc[i].next = i + 1;
    }
    vblk_vq.free_head = 0;
    vblk_vq.avail_idx = 0;
    vblk_vq.last_used_idx = 0;
    vblk_vq.queue_index = 0;
    
    /* 通知驱动已就绪 */
    outb(vblk_io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_DRIVER_OK);
    
    /* 读取磁盘容量 */
    uint64_t capacity = 0;
    for (int i = 0; i < 8; i++) {
        capacity |= ((uint64_t)inb(vblk_io_base + VIRTIO_PCI_CONFIG + i) << (i * 8));
    }
    
    vblk_config.capacity = capacity;
    
    serial_print("[VIRTIO] Disk capacity: ");
    char cbuf[30]; int cp = 0;
    uint64_t tmp = capacity;
    if (tmp == 0) cbuf[cp++] = '0';
    else { char t[20]; int tp = 0; while (tmp > 0) { t[tp++] = '0' + (tmp % 10); tmp /= 10; } while (tp > 0) cbuf[cp++] = t[--tp]; }
    cbuf[cp++] = ' '; cbuf[cp++] = 's'; cbuf[cp++] = 'e'; cbuf[cp++] = 'c'; cbuf[cp++] = 't'; cbuf[cp++] = 'o'; cbuf[cp++] = 'r'; cbuf[cp++] = 's';
    cbuf[cp++] = ' '; cbuf[cp++] = '(';
    uint64_t mb = (capacity * 512) / (1024 * 1024);
    tmp = mb;
    if (tmp == 0) cbuf[cp++] = '0';
    else { char t[20]; int tp = 0; while (tmp > 0) { t[tp++] = '0' + (tmp % 10); tmp /= 10; } while (tp > 0) cbuf[cp++] = t[--tp]; }
    cbuf[cp++] = ' '; cbuf[cp++] = 'M'; cbuf[cp++] = 'B'; cbuf[cp++] = ')';
    cbuf[cp] = '\n'; cbuf[cp+1] = '\0';
    serial_print(cbuf);
    
    virtio_blk_initialized = 1;
    serial_print("[VIRTIO] virtio-blk initialized successfully\n");
    return 0;
}

/* 检查 virtio-blk 是否可用 */
int virtio_blk_is_available(void) {
    return virtio_blk_initialized;
}

/* 获取磁盘容量 (扇区数) */
uint64_t virtio_blk_get_capacity(void) {
    return vblk_config.capacity;
}

/* 添加描述符到 VirtQueue */
static uint16_t virtq_add_desc(uint64_t addr, uint32_t len, uint16_t flags, uint16_t next) {
    uint16_t head = vblk_vq.free_head;
    
    vblk_vq.desc[head].addr = addr;
    vblk_vq.desc[head].len = len;
    vblk_vq.desc[head].flags = flags;
    vblk_vq.desc[head].next = next;
    
    /* 更新 free_head */
    if (flags & 0x1) {  /* VIRTQ_DESC_F_NEXT */
        vblk_vq.free_head = next;
    } else {
        /* 查找下一个空闲描述符 */
        for (uint16_t i = 0; i < VIRTIO_RING_SIZE; i++) {
            if (vblk_vq.desc[i].len == 0 && !(vblk_vq.desc[i].flags & 0x8)) {  /* 未使用且不在链中 */
                vblk_vq.free_head = i;
                break;
            }
        }
    }
    
    return head;
}

/* 读取扇区 - 使用低地址 DMA 缓冲区确保设备可访问 */
int virtio_blk_read_sector(uint64_t lba, void *buf) {
    if (!virtio_blk_initialized) {
        return -1;
    }

    /* 分配低地址（<4GB）DMA 缓冲区，确保 VirtIO 传统设备可访问 */
    uint64_t dma_phys = pmm_alloc_page();
    if (!dma_phys) {
        serial_print("[VIRTIO] Failed to allocate DMA page\n");
        return -1;
    }
    /* 检查物理地址是否在 32 位可寻址范围内 */
    if (dma_phys >= 0x100000000ULL) {
        serial_print("[VIRTIO] DMA page above 4GB, freeing and retrying\n");
        pmm_free_page(dma_phys);
        /* 尝试分配低地址页面 - 简单实现：直接失败 */
        return -1;
    }

    uint64_t req_virt = (uint64_t)phys_to_virt(dma_phys);
    uint64_t req_phys = dma_phys;
    uint64_t buf_phys = dma_phys + 2048;  /* 数据缓冲区在偏移 2048 */

    /* 构造请求 */
    struct virtio_blk_req *req = (struct virtio_blk_req *)req_virt;
    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = lba;

    /* 状态字节在请求后 */
    *(uint8_t *)(req_virt + sizeof(struct virtio_blk_req)) = 0xFF;

    /* 设置描述符 */
    /* 描述符0: 请求头 (设备读) */
    vblk_vq.desc[0].addr = req_phys;
    vblk_vq.desc[0].len = sizeof(struct virtio_blk_req);
    vblk_vq.desc[0].flags = 0x1;  /* VIRTQ_DESC_F_NEXT */
    vblk_vq.desc[0].next = 1;

    /* 描述符1: 数据缓冲区 (设备写) */
    vblk_vq.desc[1].addr = buf_phys;
    vblk_vq.desc[1].len = 512;
    vblk_vq.desc[1].flags = 0x3;  /* VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE */
    vblk_vq.desc[1].next = 2;

    /* 描述符2: 状态字节 (设备写) */
    vblk_vq.desc[2].addr = req_phys + sizeof(struct virtio_blk_req);
    vblk_vq.desc[2].len = 1;
    vblk_vq.desc[2].flags = 0x2;  /* VIRTQ_DESC_F_WRITE */
    vblk_vq.desc[2].next = 0;

    /* 添加描述符到 avail ring */
    uint16_t idx = vblk_vq.avail->idx % VIRTIO_RING_SIZE;
    vblk_vq.avail->ring[idx] = 0;  /* 描述符链从0开始 */

    /* 内存屏障 */
    asm volatile("mfence" ::: "memory");

    /* 更新 avail idx */
    vblk_vq.avail->idx++;

    /* 通知设备 */
    outw(vblk_io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    /* 等待完成 (带超时) */
    uint32_t timeout = 10000000;  /* 增加超时时间 */
    while (vblk_vq.used->idx == vblk_vq.last_used_idx && timeout > 0) {
        timeout--;
        asm volatile("pause");
    }

    if (timeout == 0) {
        serial_print("[VIRTIO] Read timeout!\n");
        pmm_free_page(dma_phys);
        return -1;
    }

    vblk_vq.last_used_idx = vblk_vq.used->idx;

    /* 读取状态 */
    uint8_t status = *(uint8_t *)(req_virt + sizeof(struct virtio_blk_req));

    /* 复制数据到调用者缓冲区 */
    if (status == VIRTIO_BLK_S_OK) {
        memcpy(buf, (void *)(req_virt + 2048), 512);
    }

    /* 释放 DMA 页面 */
    pmm_free_page(dma_phys);

    return (status == VIRTIO_BLK_S_OK) ? 0 : -1;
}

/* 写入扇区 */
int virtio_blk_write_sector(uint64_t lba, const void *buf) {
    if (!virtio_blk_initialized) {
        return -1;
    }
    
    /* 构造请求 */
    struct virtio_blk_req req;
    req.type = VIRTIO_BLK_T_OUT;
    req.reserved = 0;
    req.sector = lba;
    
    uint8_t status = 0xFF;
    
    /* 使用物理页面存储请求和状态 */
    uint64_t req_phys = pmm_alloc_page();
    if (!req_phys) return -1;
    
    uint64_t req_virt = (uint64_t)phys_to_virt(req_phys);
    
    /* 复制请求头和数据 */
    memcpy((void *)req_virt, &req, sizeof(req));
    memcpy((void *)(req_virt + sizeof(req) + 1), buf, 512);
    
    /* 设置描述符 */
    /* 描述符0: 请求头 (设备读) */
    vblk_vq.desc[0].addr = req_phys;
    vblk_vq.desc[0].len = sizeof(req);
    vblk_vq.desc[0].flags = 0x1;  /* VIRTQ_DESC_F_NEXT */
    vblk_vq.desc[0].next = 1;
    
    /* 描述符1: 数据缓冲区 (设备读，host→device方向，不应设WRITE标志) */
    vblk_vq.desc[1].addr = req_phys + sizeof(req) + 1;
    vblk_vq.desc[1].len = 512;
    vblk_vq.desc[1].flags = 0x1;  /* VIRTQ_DESC_F_NEXT only - 关键修复：写操作数据方向是host→device，不应设WRITE */
    vblk_vq.desc[1].next = 2;
    
    /* 描述符2: 状态字节 (设备写) */
    vblk_vq.desc[2].addr = req_phys + sizeof(req);
    vblk_vq.desc[2].len = 1;
    vblk_vq.desc[2].flags = 0x2;  /* VIRTQ_DESC_F_WRITE */
    vblk_vq.desc[2].next = 0;
    
    /* 清除状态 */
    *(uint8_t *)(req_virt + sizeof(req)) = 0xFF;
    
    /* 添加描述符到 avail ring */
    uint16_t idx = vblk_vq.avail->idx % VIRTIO_RING_SIZE;
    vblk_vq.avail->ring[idx] = 0;  /* 描述符链从0开始 */
    
    /* 内存屏障 */
    asm volatile("mfence" ::: "memory");
    
    /* 更新 avail idx */
    vblk_vq.avail->idx++;
    
    /* 通知设备 */
    outw(vblk_io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);
    
    /* 等待完成 (带超时) */
    uint32_t timeout = 1000000;
    while (vblk_vq.used->idx == vblk_vq.last_used_idx && timeout > 0) {
        timeout--;
        asm volatile("pause");
    }
    
    if (timeout == 0) {
        serial_print("[VIRTIO] Write timeout!\n");
        pmm_free_page(req_phys);
        return -1;
    }
    
    vblk_vq.last_used_idx = vblk_vq.used->idx;
    
    /* 读取状态 */
    status = *(uint8_t *)(req_virt + sizeof(req));
    
    /* 释放物理页面 */
    pmm_free_page(req_phys);
    
    return (status == VIRTIO_BLK_S_OK) ? 0 : -1;
}
