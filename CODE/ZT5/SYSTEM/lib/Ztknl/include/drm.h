/*
 * Zratro OS - DRM/KMS Graphics Framework
 * 
 * Direct Rendering Manager / Kernel Mode Setting
 */

#ifndef DRM_H
#define DRM_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "scheduler.h"

/* IOCTL 宏定义 */
#ifndef _IO
#define _IO(type, nr)        (((type) << 8) | (nr))
#define _IOR(type, nr, t)    (((type) << 8) | (nr))
#define _IOW(type, nr, t)    (((type) << 8) | (nr))
#define _IOWR(type, nr, t)   (((type) << 8) | (nr))
#endif

/* 前向声明 */
struct drm_device;
struct drm_driver;
struct drm_file;
struct drm_crtc;
struct drm_encoder;
struct drm_connector;
struct drm_plane;
struct drm_framebuffer;
struct drm_gem_object;
struct drm_property;
struct drm_mode_modeinfo;
struct drm_mode_set;
struct drm_mode_create_dumb;
struct drm_mode_fb_cmd2;

/* DRM IOCTL 命令 */
#define DRM_IOCTL_BASE          'd'
#define DRM_IO(nr)              _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOR(nr,type)        _IOR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW(nr,type)        _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr,type)       _IOWR(DRM_IOCTL_BASE, nr, type)

/* DRM IOCTL 编号 */
#define DRM_IOCTL_VERSION           DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_RESOURCES     DRM_IOR(0x01, struct drm_resources)
#define DRM_IOCTL_GET_CONNECTOR     DRM_IOR(0x02, struct drm_connector)
#define DRM_IOCTL_SET_CRTC          DRM_IOW(0x03, struct drm_crtc_request)
#define DRM_IOCTL_GET_CRTC          DRM_IOR(0x04, struct drm_crtc)
#define DRM_IOCTL_MODE_ADDFB        DRM_IOWR(0x05, struct drm_framebuffer)
#define DRM_IOCTL_MODE_RMFB         DRM_IOW(0x06, uint32_t)

/* DRM 版本 */
#define DRM_VERSION_MAJOR       1
#define DRM_VERSION_MINOR       0
#define DRM_VERSION_PATCH       0

/* GEM 对象标志 */
#define GEM_FLAG_CACHED         (1 << 0)
#define GEM_FLAG_WC             (1 << 1)  /* Write-combining */
#define GEM_FLAG_COHERENT       (1 << 2)

/* ========== DRM 核心数据结构 ========== */

/* DRM 设备 */
struct drm_device {
    char name[32];
    int minor;
    int primary;            /* 是否为主 GPU */
    
    /* 内存信息 */
    uint64_t vram_start;
    uint64_t vram_size;
    uint64_t gtt_start;
    uint64_t gtt_size;
    
    /* 驱动 */
    struct drm_driver *driver;
    void *driver_private;
    
    /* GEM 管理 */
    struct list_head gem_objects;
    struct mutex gem_lock;
    uint64_t gem_count;
    uint64_t gem_size;
    
    /* KMS */
    struct list_head crtcs;
    struct list_head encoders;
    struct list_head connectors;
    struct list_head planes;
    struct list_head framebuffers;
    
    /* 模式配置 */
    int min_width, max_width;
    int min_height, max_height;
    int preferred_depth;
    
    /* 文件 */
    struct list_head filelist;
    struct mutex file_lock;
    uint32_t file_count;
    
    /* 事件 */
    spinlock_t event_lock;
    struct list_head vblank_handlers;
    
    /* VBlank 计数器 */
    uint64_t vblank_counter[16];
};

/* DRM 驱动 */
struct drm_driver {
    const char *name;
    const char *desc;
    int major, minor, patchlevel;
    
    /* 功能标志 */
    uint32_t features;
    #define DRIVER_USE_GEM          (1 << 0)
    #define DRIVER_USE_KMS          (1 << 1)
    #define DRIVER_GEM              (1 << 2)
    #define DRIVER_PRIME            (1 << 3)
    #define DRIVER_RENDER           (1 << 4)
    #define DRIVER_ATOMIC           (1 << 5)
    
    /* GEM 操作 */
    struct drm_gem_object *(*gem_create)(struct drm_device *dev, 
                                          size_t size, uint32_t flags);
    void (*gem_free)(struct drm_gem_object *obj);
    int (*gem_open)(struct drm_gem_object *obj, struct drm_file *file);
    void (*gem_close)(struct drm_gem_object *obj, struct drm_file *file);
    int (*gem_mmap)(struct drm_gem_object *obj, uint64_t *offset);
    void *(*gem_vmap)(struct drm_gem_object *obj);
    void (*gem_vunmap)(struct drm_gem_object *obj, void *vaddr);
    
    /* DUMB 缓冲区 */
    int (*dumb_create)(struct drm_file *file, struct drm_device *dev,
                        struct drm_mode_create_dumb *args);
    int (*dumb_map_offset)(struct drm_file *file, struct drm_device *dev,
                           uint32_t handle, uint64_t *offset);
    int (*dumb_destroy)(struct drm_file *file, struct drm_device *dev,
                        uint32_t handle);
    
    /* KMS 操作 */
    int (*mode_config_init)(struct drm_device *dev);
    int (*mode_valid)(struct drm_device *dev, const struct drm_mode_modeinfo *mode);
    
    /* 页面翻转 */
    int (*page_flip)(struct drm_crtc *crtc, struct drm_framebuffer *fb,
                     struct drm_pending_flip *pending, uint32_t flags);
    
    /* 原子模式设置 */
    int (*atomic_check)(struct drm_device *dev, void *state);
    int (*atomic_commit)(struct drm_device *dev, void *state);
    
    /* 挂起/恢复 */
    int (*suspend)(struct drm_device *dev);
    int (*resume)(struct drm_device *dev);
    
    /* 设备操作 */
    int (*open)(struct drm_device *dev, struct drm_file *file);
    void (*postclose)(struct drm_device *dev, struct drm_file *file);
};

/* DRM 文件 */
struct drm_file {
    uint32_t id;
    struct drm_device *dev;
    
    /* GEM 对象句柄表 */
    struct idr object_idr;
    spinlock_t table_lock;
    
    /* 权限 */
    int authenticated;
    int master;
    int render_only;
    
    /* 事件 */
    struct list_head event_list;
    wait_queue_head_t event_wait;
    
    struct list_head lhead;
};

/* ========== GEM (Graphics Execution Manager) ========== */

/* GEM 对象 */
struct drm_gem_object {
    uint64_t size;
    uint32_t flags;
    uint32_t handle_count;
    
    /* 内存 */
    uint64_t phys_addr;
    void *vaddr;
    
    /* 引用计数 */
    struct kref refcount;
    
    /* 所属设备 */
    struct drm_device *dev;
    
    /* DMA 缓冲区 */
    struct dma_buf *dma_buf;
    
    /* 链表 */
    struct list_head head;
    
    /* 驱动私有数据 */
    void *driver_private;
};

/* GEM 创建参数 */
struct drm_gem_create {
    uint64_t size;
    uint32_t flags;
    uint32_t handle;
};

/* ========== KMS (Kernel Mode Setting) ========== */

/* 显示模式 */
struct drm_mode_modeinfo {
    struct list_head head;      /* 用于链表 */
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
};

/* CRTC (Cathode Ray Tube Controller) */
struct drm_crtc {
    uint32_t id;
    struct drm_device *dev;
    
    /* 当前状态 */
    int enabled;
    uint32_t x, y;
    uint32_t fb_id;
    uint32_t gamma_size;
    uint32_t index;
    
    /* 模式 */
    struct drm_mode_modeinfo mode;
    
    /* 连接的编码器 */
    struct drm_encoder *encoder;
    
    /* 光标 */
    struct {
        uint32_t enabled;
        int32_t x, y;
        uint32_t width, height;
        uint32_t hot_x, hot_y;
        struct drm_gem_object *obj;
    } cursor;
    
    /* 驱动操作 */
    struct drm_crtc_funcs *funcs;
    void *driver_private;
    
    struct list_head head;
};

struct drm_crtc_funcs {
    int (*set_config)(struct drm_mode_set *set);
    int (*page_flip)(struct drm_crtc *crtc, uint32_t fb_id,
                     uint32_t flags, void *user_data);
    int (*gamma_set)(struct drm_crtc *crtc, uint16_t *r, uint16_t *g, uint16_t *b,
                     uint32_t size);
    void (*destroy)(struct drm_crtc *crtc);
};

/* 编码器 */
struct drm_encoder {
    uint32_t id;
    struct drm_device *dev;
    
    int encoder_type;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
    
    struct drm_crtc *crtc;
    struct drm_connector *connector;
    
    struct drm_encoder_funcs *funcs;
    void *driver_private;
    
    struct list_head head;
};

struct drm_encoder_funcs {
    void (*destroy)(struct drm_encoder *encoder);
};

/* 连接器状态 */
enum drm_connector_status {
    DRM_MODE_CONNECTED,
    DRM_MODE_DISCONNECTED,
    DRM_MODE_UNKNOWNCONNECTION
};

/* 子像素顺序 */
enum drm_subpixel {
    DRM_MODE_SUBPIXEL_UNKNOWN = 0,
    DRM_MODE_SUBPIXEL_HORIZONTAL_RGB,
    DRM_MODE_SUBPIXEL_HORIZONTAL_BGR,
    DRM_MODE_SUBPIXEL_VERTICAL_RGB,
    DRM_MODE_SUBPIXEL_VERTICAL_BGR
};

/* 连接器 */
struct drm_connector {
    uint32_t id;
    struct drm_device *dev;
    
    int connector_type;
    int connector_type_id;
    
    /* 连接状态 */
    enum drm_connector_status status;
    
    /* 显示信息 */
    int width_mm, height_mm;
    struct list_head modes;
    int count_modes;
    
    /* 子像素顺序 */
    enum drm_subpixel subpixel;
    
    /* 连接的编码器 */
    struct drm_encoder *encoder;
    struct list_head encoder_ids;
    
    /* CRTC 关联 */
    struct drm_crtc *crtc;
    
    /* 属性 */
    uint32_t dpms;
    
    struct drm_connector_funcs *funcs;
    void *driver_private;
    
    struct list_head head;
};

struct drm_connector_funcs {
    enum drm_connector_status (*detect)(struct drm_connector *connector, int force);
    int (*fill_modes)(struct drm_connector *connector, uint32_t max_width, uint32_t max_height);
    int (*set_property)(struct drm_connector *connector, struct drm_property *property, uint64_t value);
    void (*destroy)(struct drm_connector *connector);
};

/* 平面 */
struct drm_plane {
    uint32_t id;
    struct drm_device *dev;
    
    int plane_type;
    #define DRM_PLANE_TYPE_OVERLAY  0
    #define DRM_PLANE_TYPE_PRIMARY  1
    #define DRM_PLANE_TYPE_CURSOR   2
    
    uint32_t possible_crtcs;
    uint32_t format_count;
    uint32_t *format_types;
    
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb;
    uint32_t fb_id;
    uint32_t crtc_x, crtc_y;
    uint32_t crtc_w, crtc_h;
    uint32_t src_x, src_y;
    uint32_t src_w, src_h;
    uint32_t x, y;
    uint32_t rotation;
    
    struct drm_plane_funcs *funcs;
    void *driver_private;
    
    struct list_head head;
};

struct drm_plane_funcs {
    int (*update_plane)(struct drm_plane *plane, struct drm_crtc *crtc,
                        uint32_t fb_id, uint32_t crtc_x, uint32_t crtc_y,
                        uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    int (*disable_plane)(struct drm_plane *plane);
    void (*destroy)(struct drm_plane *plane);
};

/* 帧缓冲 */
struct drm_framebuffer {
    uint32_t id;
    struct drm_device *dev;
    
    uint32_t width, height;
    uint32_t depth, bits_per_pixel;
    uint32_t pixel_format;
    
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier;
    
    struct drm_gem_object *obj[4];
    
    struct list_head head;
    struct kref refcount;
};

/* 模式设置请求 */
struct drm_mode_set {
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb;
    uint32_t x, y;
    struct drm_mode_modeinfo *mode;
    uint32_t num_connectors;
    struct drm_connector **connectors;
};

/* ========== IOCTL 数据结构 ========== */

/* 版本信息 */
struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

/* 获取资源 */
struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
};

/* 获取连接器信息 */
struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    
    uint32_t connection;
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
};

/* 创建 dumb 缓冲区 */
struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

/* 帧缓冲区创建命令 */
struct drm_mode_fb_cmd2 {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

/* 属性 */
struct drm_property {
    uint32_t id;
    char name[32];
    uint32_t flags;
    uint32_t num_values;
    uint64_t *values;
    struct list_head enum_list;
    struct list_head head;
};

/* 设置 CRTC */
struct drm_mode_crtc {
    uint32_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t connector_id;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

/* Page Flip */
struct drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
};

/* VBlank 处理程序 */
struct drm_vblank_handler {
    struct list_head link;
    uint32_t crtc_id;
    void (*callback)(uint32_t crtc_id, uint64_t timestamp, void *data);
    void *data;
};

/* DRM 属性 ID */
#define DRM_PROP_ACTIVE         1
#define DRM_PROP_MODE_ID        2
#define DRM_PROP_CRTC_ID        3
#define DRM_PROP_FB_ID          4
#define DRM_PROP_SRC_X          5
#define DRM_PROP_SRC_Y          6
#define DRM_PROP_SRC_W          7
#define DRM_PROP_SRC_H          8
#define DRM_PROP_CRTC_X         9
#define DRM_PROP_CRTC_Y         10
#define DRM_PROP_CRTC_W         11
#define DRM_PROP_CRTC_H         12

/* ========== DRM 核心 API ========== */

/* 初始化 DRM 子系统 */
int drm_init(void);

/* 注册 DRM 设备 */
int drm_dev_register(struct drm_device *dev);

/* 注销 DRM 设备 */
void drm_dev_unregister(struct drm_device *dev);

/* 创建 DRM 设备 */
struct drm_device *drm_dev_alloc(struct drm_driver *driver);

/* 释放 DRM 设备 */
void drm_dev_free(struct drm_device *dev);

/* ========== GEM API ========== */

/* 创建 GEM 对象 */
struct drm_gem_object *drm_gem_object_create(struct drm_device *dev, 
                                              size_t size, uint32_t flags);

/* 销毁 GEM 对象 */
void drm_gem_object_destroy(struct drm_gem_object *obj);

/* 引用管理 */
void drm_gem_object_get(struct drm_gem_object *obj);
void drm_gem_object_put(struct drm_gem_object *obj);

/* 查找 GEM 对象 */
struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file, uint32_t handle);

/* 句柄管理 */
int drm_gem_handle_create(struct drm_file *file, struct drm_gem_object *obj, uint32_t *handle);
int drm_gem_handle_delete(struct drm_file *file, uint32_t handle);

/* 内存映射 */
int drm_gem_mmap(struct drm_file *file, uint32_t handle, uint64_t *offset);

/* ========== KMS API ========== */

/* 模式配置初始化 */
int drm_mode_config_init(struct drm_device *dev);

/* CRTC 操作 */
int drm_crtc_init(struct drm_device *dev, struct drm_crtc *crtc,
                  struct drm_crtc_funcs *funcs);
void drm_crtc_cleanup(struct drm_crtc *crtc);

/* 编码器操作 */
int drm_encoder_init(struct drm_device *dev, struct drm_encoder *encoder,
                     struct drm_encoder_funcs *funcs, int encoder_type);
void drm_encoder_cleanup(struct drm_encoder *encoder);

/* 连接器操作 */
int drm_connector_init(struct drm_device *dev, struct drm_connector *connector,
                       struct drm_connector_funcs *funcs, int connector_type);
void drm_connector_cleanup(struct drm_connector *connector);

/* 平面操作 */
int drm_plane_init(struct drm_device *dev, struct drm_plane *plane,
                   uint32_t possible_crtcs, struct drm_plane_funcs *funcs,
                   const uint32_t *formats, uint32_t format_count,
                   int plane_type);
void drm_plane_cleanup(struct drm_plane *plane);

/* 帧缓冲操作 */
struct drm_framebuffer *drm_framebuffer_create(struct drm_device *dev,
                                                struct drm_mode_fb_cmd2 *mode_cmd);
void drm_framebuffer_destroy(struct drm_framebuffer *fb);
struct drm_framebuffer *drm_framebuffer_lookup(struct drm_device *dev, uint32_t id);

/* 模式设置 */
int drm_mode_set_config(struct drm_mode_set *set);

/* 检测连接器 */
enum drm_connector_status drm_connector_detect(struct drm_connector *connector, int force);

/* 获取模式 */
int drm_connector_fill_modes(struct drm_connector *connector, 
                              uint32_t max_width, uint32_t max_height);

/* ========== IOCTL 处理 ========== */

/* DRM 核心 IOCTL */
int drm_ioctl(struct drm_file *file, unsigned int cmd, unsigned long arg);

#endif /* DRM_H */
