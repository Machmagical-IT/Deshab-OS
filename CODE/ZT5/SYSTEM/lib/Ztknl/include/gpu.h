/*
 * Zratro OS - GPU 2D 加速驱动
 *
 * 功能: Bochs VBE 2D 加速 (QEMU 兼容), 矩形填充, BitBLT, GEM 对象管理
 */

#ifndef GPU_H
#define GPU_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== Bochs VBE PCI 标识 ========== */
#define GPU_VENDOR_BOCHS    0x1234
#define GPU_DEVICE_VBE      0x1111

/* ========== Bochs VBE MMIO 寄存器 ========== */
#define VBE_DISPI_INDEX             0x01CE
#define VBE_DISPI_DATA              0x01CF

/* VBE 寄存器索引 */
#define VBE_DISPI_INDEX_ID          0x00
#define VBE_DISPI_INDEX_XRES        0x01
#define VBE_DISPI_INDEX_YRES        0x02
#define VBE_DISPI_INDEX_BPP         0x03
#define VBE_DISPI_INDEX_ENABLE      0x04
#define VBE_DISPI_INDEX_BANK        0x05
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x06
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x07
#define VBE_DISPI_INDEX_X_OFFSET    0x08
#define VBE_DISPI_INDEX_Y_OFFSET    0x09
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0x0A

/* VBE 使能位 */
#define VBE_DISPI_ENABLED           0x01
#define VBE_DISPI_GETCAPS           0x02
#define VBE_DISPI_8BIT_DAC          0x20
#define VBE_DISPI_LFB_ENABLED       0x40
#define VBE_DISPI_NOCLEARMEM        0x80

/* ========== 2D 加速操作 ========== */
#define GPU_OP_FILL_RECT    0x01    /* 矩形填充 */
#define GPU_OP_BITBLT       0x02    /* 位块传输 (复制) */
#define GPU_OP_LINE         0x03    /* 画线 */

/* ========== 像素格式 ========== */
#define GPU_FORMAT_ARGB32   0       /* 32位 ARGB */
#define GPU_FORMAT_RGB24    1       /* 24位 RGB */
#define GPU_FORMAT_RGB565   2       /* 16位 RGB565 */

/* ========== GEM 对象 ========== */
struct gpu_gem_object {
    uint32_t id;                     /* 对象 ID */
    uint32_t width;                  /* 宽度 */
    uint32_t height;                 /* 高度 */
    uint32_t bpp;                    /* 每像素位数 */
    uint32_t pitch;                  /* 行字节数 */
    uint64_t size;                   /* 总大小 */
    uint64_t vaddr;                  /* 虚拟地址 */
    uint64_t paddr;                  /* 物理地址 */
    struct kref refcount;            /* 引用计数 */
    struct list_head list;           /* 链表 */
};

/* ========== 2D 操作描述 ========== */
struct gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct gpu_fill_op {
    struct gpu_rect rect;
    uint32_t color;                  /* 填充颜色 (ARGB32) */
};

struct gpu_bitblt_op {
    struct gpu_rect src_rect;        /* 源矩形 */
    uint32_t src_x;
    uint32_t src_y;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t width;
    uint32_t height;
};

/* ========== GPU 设备结构 ========== */
struct gpu_device {
    /* PCI 信息 */
    uint8_t pci_bus;
    uint8_t pci_dev;
    uint8_t pci_func;

    /* 帧缓冲 */
    uint64_t fb_phys;                /* 帧缓冲物理地址 */
    uint64_t fb_virt;                /* 帧缓冲虚拟地址 */
    uint64_t fb_size;                /* 帧缓冲大小 */

    /* 显示模式 */
    uint32_t xres;
    uint32_t yres;
    uint32_t bpp;
    uint32_t pitch;                  /* 行字节数 */
    int enabled;

    /* GEM 对象管理 */
    struct gpu_gem_object *gem_objects;
    int gem_count;
    uint32_t next_gem_id;
    spinlock_t gem_lock;

    /* 锁 */
    spinlock_t lock;

    /* 统计 */
    uint64_t fill_count;
    uint64_t blit_count;
};

/* ========== API 函数声明 ========== */

/* 初始化 GPU 驱动 */
int gpu_init(void);

/* 探测 Bochs VBE */
int gpu_probe(void);

/* 模式设置 */
int gpu_set_mode(uint32_t xres, uint32_t yres, uint32_t bpp);
int gpu_get_mode(uint32_t *xres, uint32_t *yres, uint32_t *bpp);

/* 2D 加速操作 */
int gpu_fill_rect(const struct gpu_fill_op *op);
int gpu_bitblt(const struct gpu_bitblt_op *op);
int gpu_draw_line(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, uint32_t color);

/* 帧缓冲访问 */
void *gpu_get_framebuffer(void);
uint64_t gpu_get_fb_size(void);

/* GEM 对象管理 */
struct gpu_gem_object *gpu_gem_create(uint32_t width, uint32_t height, uint32_t bpp);
void gpu_gem_destroy(struct gpu_gem_object *obj);
struct gpu_gem_object *gpu_gem_lookup(uint32_t id);
void gpu_gem_get(struct gpu_gem_object *obj);
void gpu_gem_put(struct gpu_gem_object *obj);

/* 获取设备实例 */
struct gpu_device *gpu_get_device(void);

#endif /* GPU_H */
