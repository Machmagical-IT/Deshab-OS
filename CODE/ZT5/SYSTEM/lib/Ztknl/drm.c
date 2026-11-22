/*
 * Zratro OS - DRM/KMS Graphics Framework Implementation
 */

#include "drm.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"

/* DRM 设备列表 */
#define MAX_DRM_DEVICES  4
static struct drm_device *drm_devices[MAX_DRM_DEVICES];
static int drm_device_count = 0;
static spinlock_t drm_lock = SPINLOCK_INIT;

/* ID 分配器 */
static uint32_t next_object_id = 1;
static uint32_t next_file_id = 1;

/* ========== 辅助函数 ========== */

static uint32_t alloc_object_id(void) {
    return next_object_id++;
}

/* ========== DRM 初始化 ========== */

int drm_init(void) {
    println("[DRM] Initializing DRM subsystem...");
    
    for (int i = 0; i < MAX_DRM_DEVICES; i++) {
        drm_devices[i] = NULL;
    }
    drm_device_count = 0;
    
    println("[DRM] Initialized successfully");
    return 0;
}

/* ========== DRM 设备管理 ========== */

struct drm_device *drm_dev_alloc(struct drm_driver *driver) {
    if (driver == NULL) return NULL;
    
    struct drm_device *dev = kmalloc(sizeof(*dev));
    if (dev == NULL) return NULL;
    
    memset(dev, 0, sizeof(*dev));
    
    dev->driver = driver;
    dev->minor = drm_device_count;
    
    /* 初始化链表 */
    list_init(&dev->gem_objects);
    list_init(&dev->crtcs);
    list_init(&dev->encoders);
    list_init(&dev->connectors);
    list_init(&dev->planes);
    list_init(&dev->framebuffers);
    list_init(&dev->filelist);
    list_init(&dev->vblank_handlers);
    
    /* 初始化锁 */
    mutex_init(&dev->gem_lock);
    mutex_init(&dev->file_lock);
    spinlock_init(&dev->event_lock);
    
    /* 设置模式配置默认值 */
    dev->min_width = 0;
    dev->max_width = 8192;
    dev->min_height = 0;
    dev->max_height = 8192;
    dev->preferred_depth = 24;
    
    /* 调用驱动初始化 */
    if (driver->features & DRIVER_USE_KMS) {
        if (driver->mode_config_init) {
            driver->mode_config_init(dev);
        }
    }
    
    return dev;
}

void drm_dev_free(struct drm_device *dev) {
    if (dev == NULL) return;
    
    /* 清理所有资源 */
    struct drm_connector *connector, *connector_tmp;
    list_for_each_entry_safe(connector, connector_tmp, &dev->connectors, head) {
        drm_connector_cleanup(connector);
    }
    
    struct drm_encoder *encoder, *encoder_tmp;
    list_for_each_entry_safe(encoder, encoder_tmp, &dev->encoders, head) {
        drm_encoder_cleanup(encoder);
    }
    
    struct drm_crtc *crtc, *crtc_tmp;
    list_for_each_entry_safe(crtc, crtc_tmp, &dev->crtcs, head) {
        drm_crtc_cleanup(crtc);
    }
    
    struct drm_plane *plane, *plane_tmp;
    list_for_each_entry_safe(plane, plane_tmp, &dev->planes, head) {
        drm_plane_cleanup(plane);
    }
    
    kfree(dev);
}

int drm_dev_register(struct drm_device *dev) {
    if (dev == NULL || drm_device_count >= MAX_DRM_DEVICES) {
        return -1;
    }
    
    spin_lock(&drm_lock);
    
    dev->minor = drm_device_count;
    drm_devices[drm_device_count++] = dev;
    
    if (drm_device_count == 1) {
        dev->primary = 1;
    }
    
    spin_unlock(&drm_lock);
    
    print("[DRM] Registered device: ");
    println(dev->driver->name);
    
    return 0;
}

void drm_dev_unregister(struct drm_device *dev) {
    if (dev == NULL) return;
    
    spin_lock(&drm_lock);
    
    for (int i = 0; i < drm_device_count; i++) {
        if (drm_devices[i] == dev) {
            drm_devices[i] = NULL;
            break;
        }
    }
    
    spin_unlock(&drm_lock);
}

/* ========== GEM 对象管理 ========== */

struct drm_gem_object *drm_gem_object_create(struct drm_device *dev,
                                              size_t size, uint32_t flags) {
    if (dev == NULL || dev->driver == NULL) return NULL;
    
    struct drm_gem_object *obj = NULL;
    
    if (dev->driver->gem_create) {
        obj = dev->driver->gem_create(dev, size, flags);
    }
    
    if (obj) {
        obj->dev = dev;
        obj->size = size;
        obj->flags = flags;
        obj->handle_count = 0;
        kref_init(&obj->refcount);
        
        mutex_lock(&dev->gem_lock);
        list_add_tail(&obj->head, &dev->gem_objects);
        dev->gem_count++;
        dev->gem_size += size;
        mutex_unlock(&dev->gem_lock);
    }
    
    return obj;
}

void drm_gem_object_destroy(struct drm_gem_object *obj) {
    if (obj == NULL) return;
    
    struct drm_device *dev = obj->dev;
    
    mutex_lock(&dev->gem_lock);
    list_del(&obj->head);
    dev->gem_count--;
    dev->gem_size -= obj->size;
    mutex_unlock(&dev->gem_lock);
    
    if (dev->driver && dev->driver->gem_free) {
        dev->driver->gem_free(obj);
    }
    
    kfree(obj);
}

void drm_gem_object_get(struct drm_gem_object *obj) {
    if (obj) {
        kref_get(&obj->refcount);
    }
}

void drm_gem_object_put(struct drm_gem_object *obj) {
    if (obj) {
        if (kref_put(&obj->refcount)) {
            drm_gem_object_destroy(obj);
        }
    }
}

struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file, uint32_t handle) {
    if (file == NULL) return NULL;
    
    spin_lock(&file->table_lock);
    struct drm_gem_object *obj = idr_find(&file->object_idr, handle);
    if (obj) {
        drm_gem_object_get(obj);
    }
    spin_unlock(&file->table_lock);
    
    return obj;
}

int drm_gem_handle_create(struct drm_file *file, struct drm_gem_object *obj, uint32_t *handle) {
    if (file == NULL || obj == NULL || handle == NULL) return -1;
    
    spin_lock(&file->table_lock);
    
    uint32_t id = alloc_object_id();
    int ret = idr_alloc(&file->object_idr, obj, id, id + 1);
    
    if (ret == 0) {
        *handle = id;
        obj->handle_count++;
        drm_gem_object_get(obj);
    }
    
    spin_unlock(&file->table_lock);
    
    return ret;
}

int drm_gem_handle_delete(struct drm_file *file, uint32_t handle) {
    if (file == NULL) return -1;
    
    spin_lock(&file->table_lock);
    
    struct drm_gem_object *obj = idr_find(&file->object_idr, handle);
    if (obj) {
        idr_remove(&file->object_idr, handle);
        obj->handle_count--;
        drm_gem_object_put(obj);
    }
    
    spin_unlock(&file->table_lock);
    
    return obj ? 0 : -1;
}

int drm_gem_mmap(struct drm_file *file, uint32_t handle, uint64_t *offset) {
    if (file == NULL || offset == NULL) return -1;
    
    struct drm_gem_object *obj = drm_gem_object_lookup(file, handle);
    if (obj == NULL) return -1;
    
    struct drm_device *dev = obj->dev;
    int ret = -1;
    
    if (dev->driver && dev->driver->gem_mmap) {
        ret = dev->driver->gem_mmap(obj, offset);
    }
    
    drm_gem_object_put(obj);
    return ret;
}

/* ========== KMS CRTC 管理 ========== */

int drm_crtc_init(struct drm_device *dev, struct drm_crtc *crtc,
                  struct drm_crtc_funcs *funcs) {
    if (dev == NULL || crtc == NULL) return -1;
    
    crtc->id = alloc_object_id();
    crtc->dev = dev;
    crtc->funcs = funcs;
    crtc->enabled = 0;
    crtc->gamma_size = 256;
    
    list_add_tail(&crtc->head, &dev->crtcs);
    
    return 0;
}

void drm_crtc_cleanup(struct drm_crtc *crtc) {
    if (crtc == NULL) return;
    
    list_del(&crtc->head);
    
    if (crtc->funcs && crtc->funcs->destroy) {
        crtc->funcs->destroy(crtc);
    }
}

/* ========== KMS 编码器管理 ========== */

int drm_encoder_init(struct drm_device *dev, struct drm_encoder *encoder,
                     struct drm_encoder_funcs *funcs, int encoder_type) {
    if (dev == NULL || encoder == NULL) return -1;
    
    encoder->id = alloc_object_id();
    encoder->dev = dev;
    encoder->funcs = funcs;
    encoder->encoder_type = encoder_type;
    encoder->possible_crtcs = 0xFFFFFFFF;
    encoder->possible_clones = 0;
    
    list_add_tail(&encoder->head, &dev->encoders);
    
    return 0;
}

void drm_encoder_cleanup(struct drm_encoder *encoder) {
    if (encoder == NULL) return;
    
    list_del(&encoder->head);
    
    if (encoder->funcs && encoder->funcs->destroy) {
        encoder->funcs->destroy(encoder);
    }
}

/* ========== KMS 连接器管理 ========== */

int drm_connector_init(struct drm_device *dev, struct drm_connector *connector,
                       struct drm_connector_funcs *funcs, int connector_type) {
    if (dev == NULL || connector == NULL) return -1;
    
    connector->id = alloc_object_id();
    connector->dev = dev;
    connector->funcs = funcs;
    connector->connector_type = connector_type;
    connector->connector_type_id = 1;
    connector->status = DRM_MODE_UNKNOWNCONNECTION;
    connector->dpms = 0;  /* DPMS On */
    
    list_init(&connector->modes);
    list_init(&connector->encoder_ids);
    
    list_add_tail(&connector->head, &dev->connectors);
    
    return 0;
}

void drm_connector_cleanup(struct drm_connector *connector) {
    if (connector == NULL) return;
    
    list_del(&connector->head);
    
    /* 释放模式列表 */
    struct drm_mode_modeinfo *mode, *tmp;
    list_for_each_entry_safe(mode, tmp, &connector->modes, head) {
        kfree(mode);
    }
    
    if (connector->funcs && connector->funcs->destroy) {
        connector->funcs->destroy(connector);
    }
}

/* ========== KMS 平面管理 ========== */

int drm_plane_init(struct drm_device *dev, struct drm_plane *plane,
                   uint32_t possible_crtcs, struct drm_plane_funcs *funcs,
                   const uint32_t *formats, uint32_t format_count,
                   int plane_type) {
    if (dev == NULL || plane == NULL) return -1;
    
    plane->id = alloc_object_id();
    plane->dev = dev;
    plane->funcs = funcs;
    plane->possible_crtcs = possible_crtcs;
    plane->plane_type = plane_type;
    
    /* 复制格式列表 */
    if (formats && format_count > 0) {
        plane->format_types = kmalloc(sizeof(uint32_t) * format_count);
        if (plane->format_types) {
            memcpy(plane->format_types, formats, sizeof(uint32_t) * format_count);
            plane->format_count = format_count;
        }
    }
    
    list_add_tail(&plane->head, &dev->planes);
    
    return 0;
}

void drm_plane_cleanup(struct drm_plane *plane) {
    if (plane == NULL) return;
    
    list_del(&plane->head);
    
    if (plane->format_types) {
        kfree(plane->format_types);
    }
    
    if (plane->funcs && plane->funcs->destroy) {
        plane->funcs->destroy(plane);
    }
}

/* ========== KMS 帧缓冲管理 ========== */

struct drm_framebuffer *drm_framebuffer_create(struct drm_device *dev,
                                                struct drm_mode_fb_cmd2 *mode_cmd) {
    if (dev == NULL || mode_cmd == NULL) return NULL;
    
    /* 验证参数 */
    if (mode_cmd->width < dev->min_width || mode_cmd->width > dev->max_width ||
        mode_cmd->height < dev->min_height || mode_cmd->height > dev->max_height) {
        return NULL;
    }
    
    struct drm_framebuffer *fb = kmalloc(sizeof(*fb));
    if (fb == NULL) return NULL;
    
    fb->id = alloc_object_id();
    fb->dev = dev;
    fb->width = mode_cmd->width;
    fb->height = mode_cmd->height;
    fb->pixel_format = mode_cmd->pixel_format;
    fb->modifier = mode_cmd->modifier[0];
    
    for (int i = 0; i < 4; i++) {
        fb->pitches[i] = mode_cmd->pitches[i];
        fb->offsets[i] = mode_cmd->offsets[i];
    }
    
    kref_init(&fb->refcount);
    
    list_add_tail(&fb->head, &dev->framebuffers);
    
    return fb;
}

void drm_framebuffer_destroy(struct drm_framebuffer *fb) {
    if (fb == NULL) return;
    
    list_del(&fb->head);
    
    /* 释放 GEM 对象引用 */
    for (int i = 0; i < 4; i++) {
        if (fb->obj[i]) {
            drm_gem_object_put(fb->obj[i]);
        }
    }
    
    kfree(fb);
}

struct drm_framebuffer *drm_framebuffer_lookup(struct drm_device *dev, uint32_t id) {
    if (dev == NULL) return NULL;
    
    struct drm_framebuffer *fb;
    list_for_each_entry(fb, &dev->framebuffers, head) {
        if (fb->id == id) {
            return fb;
        }
    }
    
    return NULL;
}

/* ========== 模式设置 ========== */

int drm_mode_set_config(struct drm_mode_set *set) {
    if (set == NULL || set->crtc == NULL) return -1;
    
    struct drm_crtc *crtc = set->crtc;
    
    if (crtc->funcs && crtc->funcs->set_config) {
        return crtc->funcs->set_config(set);
    }
    
    return -1;
}

/* ========== 连接器检测 ========== */

enum drm_connector_status drm_connector_detect(struct drm_connector *connector, int force) {
    if (connector == NULL) return DRM_MODE_UNKNOWNCONNECTION;
    
    if (connector->funcs && connector->funcs->detect) {
        return connector->funcs->detect(connector, force);
    }
    
    return DRM_MODE_UNKNOWNCONNECTION;
}

int drm_connector_fill_modes(struct drm_connector *connector,
                              uint32_t max_width, uint32_t max_height) {
    if (connector == NULL) return 0;
    
    if (connector->funcs && connector->funcs->fill_modes) {
        return connector->funcs->fill_modes(connector, max_width, max_height);
    }
    
    return 0;
}

/* ========== Dumb 缓冲区 ========== */

int drm_mode_create_dumb(struct drm_file *file, struct drm_device *dev,
                          struct drm_mode_create_dumb *args) {
    if (dev == NULL || args == NULL) return -1;
    
    if (dev->driver && dev->driver->dumb_create) {
        return dev->driver->dumb_create(file, dev, args);
    }
    
    return -1;
}

int drm_mode_mmap_dumb(struct drm_file *file, struct drm_device *dev,
                        uint32_t handle, uint64_t *offset) {
    if (dev == NULL || offset == NULL) return -1;
    
    if (dev->driver && dev->driver->dumb_map_offset) {
        return dev->driver->dumb_map_offset(file, dev, handle, offset);
    }
    
    return -1;
}

int drm_mode_destroy_dumb(struct drm_file *file, struct drm_device *dev,
                           uint32_t handle) {
    if (dev == NULL) return -1;
    
    if (dev->driver && dev->driver->dumb_destroy) {
        return dev->driver->dumb_destroy(file, dev, handle);
    }
    
    return -1;
}

/* ========== IOCTL 处理 ========== */

static int drm_ioctl_version(struct drm_file *file, struct drm_version *version) {
    struct drm_device *dev = file->dev;
    struct drm_driver *driver = dev->driver;
    
    version->version_major = driver->major;
    version->version_minor = driver->minor;
    version->version_patchlevel = driver->patchlevel;
    
    if (version->name) {
        strncpy(version->name, driver->name, version->name_len);
    }
    if (version->desc) {
        strncpy(version->desc, driver->desc, version->desc_len);
    }
    
    return 0;
}

static int drm_ioctl_get_resources(struct drm_file *file, struct drm_mode_card_res *res) {
    struct drm_device *dev = file->dev;
    
    res->count_fbs = 0;
    res->count_crtcs = 0;
    res->count_connectors = 0;
    res->count_encoders = 0;
    
    struct drm_framebuffer *fb;
    list_for_each_entry(fb, &dev->framebuffers, head) {
        res->count_fbs++;
        if (res->fb_id_ptr) {
            uint32_t *ids = (uint32_t *)res->fb_id_ptr;
            ids[res->count_fbs - 1] = fb->id;
        }
    }
    
    struct drm_crtc *crtc;
    list_for_each_entry(crtc, &dev->crtcs, head) {
        res->count_crtcs++;
        if (res->crtc_id_ptr) {
            uint32_t *ids = (uint32_t *)res->crtc_id_ptr;
            ids[res->count_crtcs - 1] = crtc->id;
        }
    }
    
    struct drm_connector *connector;
    list_for_each_entry(connector, &dev->connectors, head) {
        res->count_connectors++;
        if (res->connector_id_ptr) {
            uint32_t *ids = (uint32_t *)res->connector_id_ptr;
            ids[res->count_connectors - 1] = connector->id;
        }
    }
    
    struct drm_encoder *encoder;
    list_for_each_entry(encoder, &dev->encoders, head) {
        res->count_encoders++;
        if (res->encoder_id_ptr) {
            uint32_t *ids = (uint32_t *)res->encoder_id_ptr;
            ids[res->count_encoders - 1] = encoder->id;
        }
    }
    
    res->min_width = dev->min_width;
    res->max_width = dev->max_width;
    res->min_height = dev->min_height;
    res->max_height = dev->max_height;
    
    return 0;
}

static int drm_ioctl_get_connector(struct drm_file *file, 
                                    struct drm_mode_get_connector *conn) {
    struct drm_device *dev = file->dev;
    struct drm_connector *connector = NULL;
    
    /* 查找连接器 */
    list_for_each_entry(connector, &dev->connectors, head) {
        if (connector->id == conn->connector_id) {
            break;
        }
    }
    
    if (connector == NULL) return -1;
    
    /* 检测连接状态 */
    connector->status = drm_connector_detect(connector, 0);
    
    conn->connection = connector->status;
    conn->connector_type = connector->connector_type;
    conn->connector_type_id = connector->connector_type_id;
    conn->mm_width = connector->width_mm;
    conn->mm_height = connector->height_mm;
    conn->subpixel = connector->subpixel;
    
    /* 获取模式列表 */
    if (conn->modes_ptr && connector->status == DRM_MODE_CONNECTED) {
        struct drm_mode_modeinfo *mode;
        int i = 0;
        list_for_each_entry(mode, &connector->modes, head) {
            if (i < conn->count_modes) {
                memcpy((void *)conn->modes_ptr + i * sizeof(*mode), mode, sizeof(*mode));
                i++;
            }
        }
    }
    
    /* 获取编码器 ID */
    if (connector->encoder) {
        conn->encoder_id = connector->encoder->id;
    }
    
    return 0;
}

static int drm_ioctl_set_crtc(struct drm_file *file, struct drm_mode_crtc *crtc_req) {
    struct drm_device *dev = file->dev;
    struct drm_crtc *crtc = NULL;
    
    /* 查找 CRTC */
    list_for_each_entry(crtc, &dev->crtcs, head) {
        if (crtc->id == crtc_req->crtc_id) {
            break;
        }
    }
    
    if (crtc == NULL) return -1;
    
    /* 构建设置请求 */
    struct drm_mode_set set;
    set.crtc = crtc;
    set.fb = crtc_req->fb_id ? drm_framebuffer_lookup(dev, crtc_req->fb_id) : NULL;
    set.x = crtc_req->x;
    set.y = crtc_req->y;
    
    if (crtc_req->mode_valid) {
        set.mode = &crtc_req->mode;
    } else {
        set.mode = NULL;
    }
    
    /* 处理连接器 */
    if (crtc_req->count_connectors > 0 && crtc_req->set_connectors_ptr) {
        uint32_t *connector_ids = (uint32_t *)crtc_req->set_connectors_ptr;
        set.num_connectors = crtc_req->count_connectors;
        set.connectors = kmalloc(sizeof(*set.connectors) * set.num_connectors);
        
        for (uint32_t i = 0; i < set.num_connectors; i++) {
            struct drm_connector *conn;
            list_for_each_entry(conn, &dev->connectors, head) {
                if (conn->id == connector_ids[i]) {
                    set.connectors[i] = conn;
                    break;
                }
            }
        }
    }
    
    int ret = drm_mode_set_config(&set);
    
    if (set.connectors) {
        kfree(set.connectors);
    }
    
    return ret;
}

int drm_ioctl(struct drm_file *file, unsigned int cmd, unsigned long arg) {
    if (file == NULL || file->dev == NULL) return -1;
    
    switch (cmd) {
        case DRM_IOCTL_VERSION:
            return drm_ioctl_version(file, (struct drm_version *)arg);
            
        case DRM_IOCTL_GET_RESOURCES:
            return drm_ioctl_get_resources(file, (struct drm_mode_card_res *)arg);
            
        case DRM_IOCTL_GET_CONNECTOR:
            return drm_ioctl_get_connector(file, (struct drm_mode_get_connector *)arg);
            
        case DRM_IOCTL_SET_CRTC:
            return drm_ioctl_set_crtc(file, (struct drm_mode_crtc *)arg);
            
        default:
            print("[DRM] Unknown ioctl: ");
            print_hex(cmd);
            println("");
            return -1;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：高级 DRM 增强功能
 * 
 * 特性：
 * - DRM 页面翻转 (Page Flip)
 * - DRM 同步支持 (Sync)
 * - DRM 光标支持
 * - DRM 原子模式设置
 * - DRM 高级诊断
 * ═══════════════════════════════════════════════════════════════════════════ */

/* === DRM 页面翻转 === */

/* 页面翻转请求 */
struct drm_page_flip_req {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t sequence;
    uint64_t user_data;
};

/* 页面翻转事件 */
struct drm_page_flip_event {
    struct list_head link;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t sequence;
    uint64_t timestamp;
    uint64_t user_data;
    struct drm_file *file;
};

/* 待处理的页面翻转 */
static struct list_head pending_flips = LIST_HEAD_INIT(pending_flips);
static spinlock_t flip_lock = SPINLOCK_INIT;

/* 请求页面翻转 */
int drm_page_flip(struct drm_file *file, struct drm_crtc *crtc,
                  struct drm_framebuffer *fb, uint32_t flags, uint64_t user_data) {
    if (file == NULL || crtc == NULL || fb == NULL) return -1;
    
    struct drm_page_flip_event *event = kmalloc(sizeof(*event));
    if (event == NULL) return -1;
    
    event->crtc_id = crtc->id;
    event->fb_id = fb->id;
    event->sequence = 0;
    event->user_data = user_data;
    event->file = file;
    
    spin_lock(&flip_lock);
    list_add_tail(&event->link, &pending_flips);
    spin_unlock(&flip_lock);
    
    /* 如果有驱动实现的翻转 */
    struct drm_device *dev = crtc->dev;
    if (dev->driver && dev->driver->page_flip) {
        return dev->driver->page_flip(crtc, fb, NULL, flags);
    }
    
    return 0;
}

/* 处理垂直空白事件 */
void drm_handle_vblank(struct drm_device *dev, uint32_t crtc_id) {
    uint64_t timestamp;
    uint64_t tsc_val;
    asm volatile("rdtsc" : "=A"(tsc_val));
    timestamp = tsc_val;
    
    spin_lock(&flip_lock);
    
    /* 处理待处理的页面翻转 */
    struct drm_page_flip_event *event, *next;
    list_for_each_entry_safe(event, next, &pending_flips, link) {
        if (event->crtc_id == crtc_id) {
            /* 发送事件到用户空间 */
            event->timestamp = timestamp;
            event->sequence = dev->vblank_counter[crtc_id]++;
            
            /* 添加到文件的事件队列 */
            spin_lock(&dev->event_lock);
            list_add_tail(&event->link, &event->file->event_list);
            spin_unlock(&dev->event_lock);
            
            list_del(&event->link);
        }
    }
    
    /* 调用 vblank 处理程序 */
    struct drm_vblank_handler *handler;
    list_for_each_entry(handler, &dev->vblank_handlers, link) {
        if (handler->crtc_id == crtc_id && handler->callback) {
            handler->callback(crtc_id, timestamp, handler->data);
        }
    }
    
    spin_unlock(&flip_lock);
}

/* 注册 vblank 处理程序 */
int drm_vblank_handler_register(struct drm_device *dev, uint32_t crtc_id,
                                 void (*callback)(uint32_t, uint64_t, void *),
                                 void *data) {
    struct drm_vblank_handler *handler = kmalloc(sizeof(*handler));
    if (handler == NULL) return -1;
    
    handler->crtc_id = crtc_id;
    handler->callback = callback;
    handler->data = data;
    
    spin_lock(&dev->event_lock);
    list_add(&handler->link, &dev->vblank_handlers);
    spin_unlock(&dev->event_lock);
    
    return 0;
}

/* === DRM 同步支持 === */

/* 同步对象类型 */
enum drm_sync_type {
    DRM_SYNC_FENCE,         /* Fence */
    DRM_SYNC_SEMAPHORE,     /* 信号量 */
    DRM_SYNC_TIMELINE,      /* 时间线 */
};

/* 同步对象 */
struct drm_sync_obj {
    uint32_t id;
    enum drm_sync_type type;
    volatile int signaled;
    uint64_t timeline_value;
    uint64_t signaled_value;
    struct list_head callbacks;
    spinlock_t lock;
};

#define MAX_SYNC_OBJS   64
static struct drm_sync_obj *sync_objs[MAX_SYNC_OBJS];

/* 创建同步对象 */
struct drm_sync_obj *drm_sync_obj_create(enum drm_sync_type type) {
    struct drm_sync_obj *obj = kmalloc(sizeof(*obj));
    if (obj == NULL) return NULL;
    
    obj->id = alloc_object_id();
    obj->type = type;
    obj->signaled = 0;
    obj->timeline_value = 0;
    obj->signaled_value = 0;
    list_init(&obj->callbacks);
    spinlock_init(&obj->lock);
    
    for (int i = 0; i < MAX_SYNC_OBJS; i++) {
        if (sync_objs[i] == NULL) {
            sync_objs[i] = obj;
            break;
        }
    }
    
    return obj;
}

/* 销毁同步对象 */
void drm_sync_obj_destroy(struct drm_sync_obj *obj) {
    if (obj == NULL) return;
    
    for (int i = 0; i < MAX_SYNC_OBJS; i++) {
        if (sync_objs[i] == obj) {
            sync_objs[i] = NULL;
            break;
        }
    }
    
    kfree(obj);
}

/* 等待同步对象 */
int drm_sync_obj_wait(struct drm_sync_obj *obj, uint64_t timeout_ns) {
    if (obj == NULL) return -1;
    
    uint64_t start = get_tick_count();
    uint64_t deadline = start + timeout_ns;
    
    while (!obj->signaled) {
        if (timeout_ns > 0 && get_tick_count() > deadline) {
            return -1;  /* 超时 */
        }
        asm volatile("pause");
    }
    
    return 0;
}

/* 发送同步对象信号 */
void drm_sync_obj_signal(struct drm_sync_obj *obj) {
    if (obj == NULL) return;
    
    spin_lock(&obj->lock);
    obj->signaled = 1;
    obj->signaled_value = obj->timeline_value;
    
    /* 触发回调 */
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &obj->callbacks) {
        /* 调用回调 */
    }
    
    spin_unlock(&obj->lock);
}

/* === DRM 光标支持 === */

/* 光标状态 */
struct drm_cursor {
    uint32_t enabled;
    int32_t x, y;
    uint32_t width, height;
    uint32_t hot_x, hot_y;
    struct drm_gem_object *obj;
    uint32_t format;
};

/* 设置光标 */
int drm_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *obj,
                   uint32_t width, uint32_t height,
                   int32_t hot_x, int32_t hot_y) {
    if (crtc == NULL) return -1;
    
    crtc->cursor.enabled = obj ? 1 : 0;
    crtc->cursor.width = width;
    crtc->cursor.height = height;
    crtc->cursor.hot_x = hot_x;
    crtc->cursor.hot_y = hot_y;
    
    if (crtc->cursor.obj) {
        drm_gem_object_put(crtc->cursor.obj);
    }
    crtc->cursor.obj = obj;
    if (obj) {
        drm_gem_object_get(obj);
    }
    
    return 0;
}

/* 移动光标 */
int drm_cursor_move(struct drm_crtc *crtc, int32_t x, int32_t y) {
    if (crtc == NULL) return -1;
    
    crtc->cursor.x = x;
    crtc->cursor.y = y;
    
    return 0;
}

/* === DRM 原子模式设置 === */

/* 原子状态 */
struct drm_atomic_state {
    struct drm_device *dev;
    struct list_head connectors;
    struct list_head crtcs;
    struct list_head planes;
    int commit;
    int async_update;
};

/* 原子请求 */
struct drm_atomic_req {
    uint32_t obj_id;
    uint32_t prop_id;
    uint64_t value;
};

/* 创建原子状态 */
struct drm_atomic_state *drm_atomic_state_alloc(struct drm_device *dev) {
    struct drm_atomic_state *state = kmalloc(sizeof(*state));
    if (state == NULL) return NULL;
    
    state->dev = dev;
    list_init(&state->connectors);
    list_init(&state->crtcs);
    list_init(&state->planes);
    state->commit = 0;
    state->async_update = 0;
    
    return state;
}

/* 设置属性 */
int drm_atomic_set_property(struct drm_atomic_state *state,
                            uint32_t obj_id, uint32_t prop_id, uint64_t value) {
    if (state == NULL) return -1;
    
    /* 查找对象 */
    struct drm_device *dev = state->dev;
    
    /* 检查 CRTC */
    struct drm_crtc *crtc;
    list_for_each_entry(crtc, &dev->crtcs, head) {
        if (crtc->id == obj_id) {
            /* 设置 CRTC 属性 */
            switch (prop_id) {
                case DRM_PROP_ACTIVE:
                    crtc->enabled = value ? 1 : 0;
                    break;
                case DRM_PROP_MODE_ID:
                    /* 设置模式 */
                    break;
            }
            return 0;
        }
    }
    
    /* 检查连接器 */
    struct drm_connector *connector;
    list_for_each_entry(connector, &dev->connectors, head) {
        if (connector->id == obj_id) {
            switch (prop_id) {
                case DRM_PROP_CRTC_ID:
                    connector->crtc = (struct drm_crtc *)value;
                    break;
            }
            return 0;
        }
    }
    
    /* 检查平面 */
    struct drm_plane *plane;
    list_for_each_entry(plane, &dev->planes, head) {
        if (plane->id == obj_id) {
            switch (prop_id) {
                case DRM_PROP_FB_ID:
                    plane->fb = (struct drm_framebuffer *)value;
                    break;
                case DRM_PROP_CRTC_ID:
                    plane->crtc = (struct drm_crtc *)value;
                    break;
                case DRM_PROP_SRC_X:
                    plane->src_x = value;
                    break;
                case DRM_PROP_SRC_Y:
                    plane->src_y = value;
                    break;
                case DRM_PROP_SRC_W:
                    plane->src_w = value;
                    break;
                case DRM_PROP_SRC_H:
                    plane->src_h = value;
                    break;
                case DRM_PROP_CRTC_X:
                    plane->crtc_x = value;
                    break;
                case DRM_PROP_CRTC_Y:
                    plane->crtc_y = value;
                    break;
                case DRM_PROP_CRTC_W:
                    plane->crtc_w = value;
                    break;
                case DRM_PROP_CRTC_H:
                    plane->crtc_h = value;
                    break;
            }
            return 0;
        }
    }
    
    return -1;
}

/* 提交原子状态 */
int drm_atomic_commit(struct drm_atomic_state *state) {
    if (state == NULL) return -1;
    
    struct drm_device *dev = state->dev;
    
    /* 验证状态 */
    if (dev->driver && dev->driver->atomic_check) {
        int ret = dev->driver->atomic_check(dev, state);
        if (ret != 0) {
            return ret;
        }
    }
    
    /* 提交更改 */
    if (dev->driver && dev->driver->atomic_commit) {
        return dev->driver->atomic_commit(dev, state);
    }
    
    return -1;
}

/* 释放原子状态 */
void drm_atomic_state_free(struct drm_atomic_state *state) {
    if (state == NULL) return;
    kfree(state);
}

/* === DRM 属性管理 === */

#define MAX_DRM_PROPS   128

static struct drm_property drm_props[MAX_DRM_PROPS];
static uint32_t drm_prop_count;

/* 创建属性 */
struct drm_property *drm_property_create(struct drm_device *dev,
                                          const char *name, uint32_t type,
                                          uint32_t flags, uint64_t *values,
                                          uint32_t num_values) {
    if (dev == NULL || name == NULL) return NULL;
    if (drm_prop_count >= MAX_DRM_PROPS) return NULL;
    
    struct drm_property *prop = &drm_props[drm_prop_count++];
    
    prop->id = alloc_object_id();
    int i;
    for (i = 0; i < 31 && name[i]; i++) prop->name[i] = name[i];
    prop->name[i] = '\0';
    prop->flags = flags;
    prop->values = values;
    prop->num_values = num_values;
    
    return prop;
}

/* 查找属性 */
struct drm_property *drm_property_find(struct drm_device *dev, uint32_t id) {
    for (uint32_t i = 0; i < drm_prop_count; i++) {
        if (drm_props[i].id == id) {
            return &drm_props[i];
        }
    }
    return NULL;
}

/* === DRM 诊断 === */

struct drm_diagnostic {
    uint32_t device_count;
    uint32_t gem_count;
    uint64_t gem_size;
    uint32_t fb_count;
    uint32_t crtc_count;
    uint32_t connector_count;
    uint32_t encoder_count;
    uint32_t plane_count;
    uint64_t vblank_count;
    uint64_t flip_count;
};

/* 获取诊断信息 */
void drm_get_diagnostic(struct drm_diagnostic *diag) {
    if (diag == NULL) return;
    
    memset(diag, 0, sizeof(*diag));
    diag->device_count = drm_device_count;
    
    for (int i = 0; i < MAX_DRM_DEVICES; i++) {
        if (drm_devices[i]) {
            diag->gem_count += drm_devices[i]->gem_count;
            diag->gem_size += drm_devices[i]->gem_size;
            
            struct drm_framebuffer *fb;
            list_for_each_entry(fb, &drm_devices[i]->framebuffers, head) {
                diag->fb_count++;
            }
            
            struct drm_crtc *crtc;
            list_for_each_entry(crtc, &drm_devices[i]->crtcs, head) {
                diag->crtc_count++;
                diag->vblank_count += drm_devices[i]->vblank_counter[crtc->index];
            }
            
            struct drm_connector *conn;
            list_for_each_entry(conn, &drm_devices[i]->connectors, head) {
                diag->connector_count++;
            }
            
            struct drm_encoder *enc;
            list_for_each_entry(enc, &drm_devices[i]->encoders, head) {
                diag->encoder_count++;
            }
            
            struct drm_plane *plane;
            list_for_each_entry(plane, &drm_devices[i]->planes, head) {
                diag->plane_count++;
            }
        }
    }
}

/* 打印 DRM 诊断报告 */
void drm_print_diagnostic_report(void) {
    println("");
    println("╔════════════════════════════════════════════════════════════╗");
    println("║           Zratro OS DRM Diagnostic Report                  ║");
    println("╚════════════════════════════════════════════════════════════╝");
    
    println("");
    println("=== 设备统计 ===");
    
    print("设备数量: ");
    print_dec(drm_device_count);
    println("");
    
    for (int i = 0; i < MAX_DRM_DEVICES; i++) {
        if (drm_devices[i]) {
            print("  设备 ");
            print_dec(i);
            print(": ");
            println(drm_devices[i]->driver->name);
            
            print("    GEM 对象: ");
            print_dec(drm_devices[i]->gem_count);
            print(" (");
            print_dec(drm_devices[i]->gem_size / 1024);
            println(" KB)");
        }
    }
    
    println("");
    println("=== 资源统计 ===");
    
    struct drm_diagnostic diag;
    drm_get_diagnostic(&diag);
    
    print("帧缓冲: ");
    print_dec(diag.fb_count);
    println("");
    
    print("CRTC: ");
    print_dec(diag.crtc_count);
    println("");
    
    print("连接器: ");
    print_dec(diag.connector_count);
    println("");
    
    print("编码器: ");
    print_dec(diag.encoder_count);
    println("");
    
    print("平面: ");
    print_dec(diag.plane_count);
    println("");
    
    println("");
    println("=== 性能统计 ===");
    
    print("VBlank 计数: ");
    print_dec(diag.vblank_count);
    println("");
    
    /* 同步对象 */
    int sync_count = 0;
    for (int i = 0; i < MAX_SYNC_OBJS; i++) {
        if (sync_objs[i]) sync_count++;
    }
    
    print("同步对象: ");
    print_dec(sync_count);
    println("");
    
    /* 待处理翻转 */
    int flip_count = 0;
    struct list_head *pos;
    list_for_each(pos, &pending_flips) {
        flip_count++;
    }
    
    print("待处理翻转: ");
    print_dec(flip_count);
    println("");
    
    println("");
    println("════════════════════════════════════════════════════════════");
}

/* === DRM 文件操作 === */

/* 打开 DRM 设备 */
struct drm_file *drm_file_open(struct drm_device *dev) {
    if (dev == NULL) return NULL;
    
    struct drm_file *file = kmalloc(sizeof(*file));
    if (file == NULL) return NULL;
    
    memset(file, 0, sizeof(*file));
    file->dev = dev;
    file->id = next_file_id++;
    file->authenticated = 1;  /* 内核进程默认认证 */
    
    /* 分配 IDR 指针数组 */
    void **ptrs = kmalloc(sizeof(void *) * 256);
    if (ptrs == NULL) {
        kfree(file);
        return NULL;
    }
    idr_init(&file->object_idr, ptrs, 256);
    list_init(&file->event_list);
    spinlock_init(&file->table_lock);
    
    mutex_lock(&dev->file_lock);
    list_add(&file->lhead, &dev->filelist);
    dev->file_count++;
    mutex_unlock(&dev->file_lock);
    
    return file;
}

/* 关闭 DRM 文件 */
void drm_file_close(struct drm_file *file) {
    if (file == NULL) return;
    
    struct drm_device *dev = file->dev;
    
    mutex_lock(&dev->file_lock);
    list_del(&file->lhead);
    dev->file_count--;
    mutex_unlock(&dev->file_lock);
    
    /* 释放所有 GEM 对象句柄 */
    spin_lock(&file->table_lock);
    /* 简化实现：直接清空 IDR */
    memset(&file->object_idr, 0, sizeof(file->object_idr));
    spin_unlock(&file->table_lock);
    
    /* 释放待处理事件 */
    spin_lock(&dev->event_lock);
    struct drm_page_flip_event *event, *next;
    list_for_each_entry_safe(event, next, &file->event_list, link) {
        list_del(&event->link);
        kfree(event);
    }
    spin_unlock(&dev->event_lock);
    
    kfree(file);
}
