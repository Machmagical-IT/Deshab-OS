/*
 * Zratro OS - GPU 2D 加速驱动
 *
 * 功能: Bochs VBE 2D 加速 (QEMU 兼容), 矩形填充, BitBLT, GEM 对象管理
 */

#include "gpu.h"
#include "console.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "string.h"

/* ========== VBE I/O 访问 ========== */

static inline void vbe_write_index(uint16_t index) {
    uint16_t port = VBE_DISPI_INDEX;
    asm volatile("outw %w0, %w1" : : "a"(index), "Nd"(port));
}

static inline void vbe_write_data(uint16_t data) {
    uint16_t port = VBE_DISPI_DATA;
    asm volatile("outw %w0, %w1" : : "a"(data), "Nd"(port));
}

static inline uint16_t vbe_read_data(void) {
    uint16_t val;
    uint16_t port = VBE_DISPI_DATA;
    asm volatile("inw %w1, %w0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void vbe_write_reg(uint16_t index, uint16_t data) {
    vbe_write_index(index);
    vbe_write_data(data);
}

static inline uint16_t vbe_read_reg(uint16_t index) {
    vbe_write_index(index);
    return vbe_read_data();
}

/* ========== 全局设备实例 ========== */

static struct gpu_device gpu_dev;

/* ========== 帧缓冲像素操作 ========== */

static inline void gpu_set_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= gpu_dev.xres || y >= gpu_dev.yres) return;
    uint32_t *fb = (uint32_t *)gpu_dev.fb_virt;
    fb[y * (gpu_dev.pitch / 4) + x] = color;
}

static inline uint32_t gpu_get_pixel(uint32_t x, uint32_t y) {
    if (x >= gpu_dev.xres || y >= gpu_dev.yres) return 0;
    uint32_t *fb = (uint32_t *)gpu_dev.fb_virt;
    return fb[y * (gpu_dev.pitch / 4) + x];
}

/* ========== 模式设置 ========== */

int gpu_set_mode(uint32_t xres, uint32_t yres, uint32_t bpp) {
    /* 禁用显示 */
    vbe_write_reg(VBE_DISPI_INDEX_ENABLE, 0);

    /* 设置分辨率和色深 */
    vbe_write_reg(VBE_DISPI_INDEX_XRES, (uint16_t)xres);
    vbe_write_reg(VBE_DISPI_INDEX_YRES, (uint16_t)yres);
    vbe_write_reg(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);

    /* 设置虚拟分辨率 (与物理相同) */
    vbe_write_reg(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)xres);
    vbe_write_reg(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)yres);
    vbe_write_reg(VBE_DISPI_INDEX_X_OFFSET, 0);
    vbe_write_reg(VBE_DISPI_INDEX_Y_OFFSET, 0);

    /* 启用 LFB 模式 */
    vbe_write_reg(VBE_DISPI_INDEX_ENABLE,
                  VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    gpu_dev.xres = xres;
    gpu_dev.yres = yres;
    gpu_dev.bpp = bpp;
    gpu_dev.pitch = xres * (bpp / 8);
    gpu_dev.fb_size = (uint64_t)xres * yres * (bpp / 8);
    gpu_dev.enabled = 1;

    printk("gpu: 模式设置 %ux%u@%ubpp (帧缓冲=%llu字节)",
           xres, yres, bpp, gpu_dev.fb_size);
    return 0;
}

int gpu_get_mode(uint32_t *xres, uint32_t *yres, uint32_t *bpp) {
    if (xres) *xres = gpu_dev.xres;
    if (yres) *yres = gpu_dev.yres;
    if (bpp) *bpp = gpu_dev.bpp;
    return 0;
}

/* ========== 2D 加速操作 ========== */

int gpu_fill_rect(const struct gpu_fill_op *op) {
    if (op == NULL || !gpu_dev.enabled) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&gpu_dev.lock);

    uint32_t *fb = (uint32_t *)gpu_dev.fb_virt;
    uint32_t pitch_words = gpu_dev.pitch / 4;

    /* 裁剪矩形到屏幕范围 */
    uint32_t x1 = op->rect.x;
    uint32_t y1 = op->rect.y;
    uint32_t x2 = MIN(op->rect.x + op->rect.width, gpu_dev.xres);
    uint32_t y2 = MIN(op->rect.y + op->rect.height, gpu_dev.yres);

    /* 填充矩形 */
    for (uint32_t y = y1; y < y2; y++) {
        uint32_t *row = fb + y * pitch_words;
        for (uint32_t x = x1; x < x2; x++) {
            row[x] = op->color;
        }
    }

    gpu_dev.fill_count++;
    spin_unlock_irqrestore(&gpu_dev.lock, flags);
    return 0;
}

int gpu_bitblt(const struct gpu_bitblt_op *op) {
    if (op == NULL || !gpu_dev.enabled) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&gpu_dev.lock);

    uint32_t *fb = (uint32_t *)gpu_dev.fb_virt;
    uint32_t pitch_words = gpu_dev.pitch / 4;

    uint32_t sx = op->src_x;
    uint32_t sy = op->src_y;
    uint32_t dx = op->dst_x;
    uint32_t dy = op->dst_y;
    uint32_t w = op->width;
    uint32_t h = op->height;

    /* 裁剪 */
    if (sx + w > gpu_dev.xres) w = gpu_dev.xres - sx;
    if (sy + h > gpu_dev.yres) h = gpu_dev.yres - sy;
    if (dx + w > gpu_dev.xres) w = gpu_dev.xres - dx;
    if (dy + h > gpu_dev.yres) h = gpu_dev.yres - dy;

    /* 判断是否需要反向复制 (处理重叠) */
    if (sy < dy || (sy == dy && sx < dx)) {
        /* 从右下角开始复制 */
        for (int32_t row = (int32_t)h - 1; row >= 0; row--) {
            uint32_t *src_row = fb + (sy + row) * pitch_words;
            uint32_t *dst_row = fb + (dy + row) * pitch_words;
            for (int32_t col = (int32_t)w - 1; col >= 0; col--) {
                dst_row[dx + col] = src_row[sx + col];
            }
        }
    } else {
        /* 从左上角开始复制 */
        for (uint32_t row = 0; row < h; row++) {
            uint32_t *src_row = fb + (sy + row) * pitch_words;
            uint32_t *dst_row = fb + (dy + row) * pitch_words;
            for (uint32_t col = 0; col < w; col++) {
                dst_row[dx + col] = src_row[sx + col];
            }
        }
    }

    gpu_dev.blit_count++;
    spin_unlock_irqrestore(&gpu_dev.lock, flags);
    return 0;
}

int gpu_draw_line(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, uint32_t color) {
    if (!gpu_dev.enabled) return Z_ERR_NOTSUP;

    /* Bresenham 画线算法 */
    int32_t dx = (int32_t)x2 - (int32_t)x1;
    int32_t dy = (int32_t)y2 - (int32_t)y1;
    int32_t sx = dx < 0 ? -1 : 1;
    int32_t sy = dy < 0 ? -1 : 1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;

    int32_t err = dx - dy;
    int32_t e2;

    while (1) {
        gpu_set_pixel(x1, y1, color);
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 < dx) { err += dx; y1 += sy; }
    }

    return 0;
}

/* ========== 帧缓冲访问 ========== */

void *gpu_get_framebuffer(void) {
    return (void *)gpu_dev.fb_virt;
}

uint64_t gpu_get_fb_size(void) {
    return gpu_dev.fb_size;
}

/* ========== GEM 对象管理 ========== */

struct gpu_gem_object *gpu_gem_create(uint32_t width, uint32_t height, uint32_t bpp) {
    struct gpu_gem_object *obj = (struct gpu_gem_object *)kmalloc(sizeof(*obj));
    if (obj == NULL) return NULL;

    obj->id = gpu_dev.next_gem_id++;
    obj->width = width;
    obj->height = height;
    obj->bpp = bpp;
    obj->pitch = width * (bpp / 8);
    obj->size = (uint64_t)obj->pitch * height;

    /* 分配物理内存 */
    size_t pages = (obj->size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        kfree(obj);
        return NULL;
    }
    obj->paddr = phys;
    obj->vaddr = (uint64_t)phys_to_virt(phys);

    kref_init(&obj->refcount);
    INIT_LIST_HEAD(&obj->list);

    /* 添加到全局列表 */
    unsigned long flags = spin_lock_irqsave(&gpu_dev.gem_lock);
    list_add_tail(&obj->list, &gpu_dev.gem_objects->list);
    gpu_dev.gem_count++;
    spin_unlock_irqrestore(&gpu_dev.gem_lock, flags);

    return obj;
}

void gpu_gem_destroy(struct gpu_gem_object *obj) {
    if (obj == NULL) return;

    unsigned long flags = spin_lock_irqsave(&gpu_dev.gem_lock);
    list_del(&obj->list);
    gpu_dev.gem_count--;
    spin_unlock_irqrestore(&gpu_dev.gem_lock, flags);

    /* 释放物理内存 */
    if (obj->paddr != 0) {
        size_t pages = (obj->size + PAGE_SIZE - 1) / PAGE_SIZE;
        pmm_free_pages(obj->paddr, pages);
    }

    kfree(obj);
}

struct gpu_gem_object *gpu_gem_lookup(uint32_t id) {
    struct gpu_gem_object *obj;
    struct list_head *pos;

    unsigned long flags = spin_lock_irqsave(&gpu_dev.gem_lock);
    list_for_each(pos, &gpu_dev.gem_objects->list) {
        obj = list_entry(pos, struct gpu_gem_object, list);
        if (obj->id == id) {
            spin_unlock_irqrestore(&gpu_dev.gem_lock, flags);
            return obj;
        }
    }
    spin_unlock_irqrestore(&gpu_dev.gem_lock, flags);
    return NULL;
}

void gpu_gem_get(struct gpu_gem_object *obj) {
    if (obj) kref_get(&obj->refcount);
}

void gpu_gem_put(struct gpu_gem_object *obj) {
    if (obj && kref_put(&obj->refcount)) {
        gpu_gem_destroy(obj);
    }
}

/* ========== PCI 探测 ========== */

int gpu_probe(void) {
    printk("gpu: 扫描 Bochs VBE...");

    for (uint8_t bus = 0; bus < 255; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor = pci_read_config32(bus, dev, func, 0x00);
                if (vendor == 0xFFFFFFFF) continue;

                uint16_t vid = vendor & 0xFFFF;
                uint16_t did = (vendor >> 16) & 0xFFFF;

                if (vid == GPU_VENDOR_BOCHS && did == GPU_DEVICE_VBE) {
                    printk("gpu: 发现 Bochs VBE bus=%u dev=%u func=%u", bus, dev, func);
                    gpu_dev.pci_bus = bus;
                    gpu_dev.pci_dev = dev;
                    gpu_dev.pci_func = func;
                    return 0;
                }
            }
        }
    }

    printk("gpu: 未找到 Bochs VBE 设备");
    return -1;
}

/* ========== 初始化 ========== */

int gpu_init(void) {
    printk("gpu: 初始化 GPU 2D 加速驱动...");

    memset(&gpu_dev, 0, sizeof(gpu_dev));
    spinlock_init(&gpu_dev.lock);
    spinlock_init(&gpu_dev.gem_lock);

    /* 探测 PCI 设备 */
    if (gpu_probe() != 0) {
        printk("gpu: 未找到 GPU 设备");
        return -1;
    }

    /* 获取帧缓冲物理地址 (BAR0) */
    uint32_t bar0 = pci_get_bar(gpu_dev.pci_bus, gpu_dev.pci_dev, gpu_dev.pci_func, 0);
    gpu_dev.fb_phys = bar0 & ~0xF;
    gpu_dev.fb_virt = (uint64_t)phys_to_virt(gpu_dev.fb_phys);

    /* 读取 VBE ID 验证 */
    uint16_t vbe_id = vbe_read_reg(VBE_DISPI_INDEX_ID);
    printk("gpu: Bochs VBE ID=0x%04X 帧缓冲=0x%016llX", vbe_id, gpu_dev.fb_phys);

    /* 读取当前模式 */
    gpu_dev.xres = vbe_read_reg(VBE_DISPI_INDEX_XRES);
    gpu_dev.yres = vbe_read_reg(VBE_DISPI_INDEX_YRES);
    gpu_dev.bpp = vbe_read_reg(VBE_DISPI_INDEX_BPP);
    gpu_dev.pitch = gpu_dev.xres * (gpu_dev.bpp / 8);
    gpu_dev.fb_size = (uint64_t)gpu_dev.xres * gpu_dev.yres * (gpu_dev.bpp / 8);

    printk("gpu: 当前模式 %ux%u@%ubpp", gpu_dev.xres, gpu_dev.yres, gpu_dev.bpp);

    /* 初始化 GEM */
    gpu_dev.gem_count = 0;
    gpu_dev.next_gem_id = 1;

    gpu_dev.enabled = 1;
    printk("gpu: GPU 2D 加速驱动初始化完成");
    return 0;
}

struct gpu_device *gpu_get_device(void) {
    return &gpu_dev;
}
