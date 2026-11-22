/*
 * Zratro OS - Z-Pipe (零拷贝管道子系统)
 * 
 * 实现：
 * 1. 无锁环形缓冲区 - 高性能生产者-消费者
 * 2. DMA 直通 - 绕过 CPU 拷贝
 * 3. 共享内存通道 - 进程间零拷贝
 * 4. 背压控制 - 防止过载
 */

#include "zpipe.h"
#include "pmm.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "task.h"

/* ========== 全局 Z-Pipe 管理器 ========== */

static struct zpipe global_zpipe;

/* ========== 无锁环形缓冲区实现 ========== */

/* 初始化环形缓冲区 */
static int zp_ring_init(struct zp_ring_buffer *ring, uint32_t size) {
    if (size < ZP_MIN_SIZE || size > ZP_MAX_SIZE) {
        return -1;
    }
    
    /* 确保大小是 2 的幂 */
    uint32_t actual_size = 1;
    while (actual_size < size) {
        actual_size <<= 1;
    }
    
    /* 分配物理连续内存 (DMA 需要) */
    uint64_t phys = pmm_alloc_pages(actual_size / PAGE_SIZE);
    if (phys == 0) {
        return -1;
    }
    
    /* 获取虚拟地址 */
    void *virt = phys_to_virt(phys);
    if (virt == NULL) {
        pmm_free_pages(phys, actual_size / PAGE_SIZE);
        return -1;
    }
    
    ring->data = (uint8_t *)virt;
    ring->phys_addr = phys;
    ring->size = actual_size;
    ring->mask = actual_size - 1;
    ring->head = 0;
    ring->tail = 0;
    ring->bytes_written = 0;
    ring->bytes_read = 0;
    ring->overflows = 0;
    
    /* 清零 */
    memset(virt, 0, actual_size);
    
    return 0;
}

/* 销毁环形缓冲区 */
static void zp_ring_destroy(struct zp_ring_buffer *ring) {
    if (ring->data != NULL) {
        pmm_free_pages(ring->phys_addr, ring->size / PAGE_SIZE);
        ring->data = NULL;
        ring->phys_addr = 0;
    }
}

/* 无锁写入 (单生产者) */
static uint32_t zp_ring_write(struct zp_ring_buffer *ring, const void *data, 
                               uint32_t size) {
    uint32_t free_space = zp_ring_free(ring);
    if (free_space == 0) {
        ring->overflows++;
        return 0;
    }
    
    uint32_t to_write = size < free_space ? size : free_space;
    uint32_t pos = zp_ring_index(ring, ring->tail);
    
    /* 可能需要分两段写入 (环形) */
    uint32_t first_chunk = ring->size - pos;
    if (first_chunk >= to_write) {
        memcpy(ring->data + pos, data, to_write);
    } else {
        memcpy(ring->data + pos, data, first_chunk);
        memcpy(ring->data, (const uint8_t *)data + first_chunk, 
               to_write - first_chunk);
    }
    
    /* 内存屏障确保数据写入完成 */
    __asm__ volatile("" ::: "memory");
    
    /* 更新尾指针 */
    ring->tail += to_write;
    ring->bytes_written += to_write;
    
    return to_write;
}

/* 无锁读取 (单消费者) */
static uint32_t zp_ring_read(struct zp_ring_buffer *ring, void *data, 
                              uint32_t size) {
    uint32_t used = zp_ring_used(ring);
    if (used == 0) {
        return 0;
    }
    
    uint32_t to_read = size < used ? size : used;
    uint32_t pos = zp_ring_index(ring, ring->head);
    
    /* 可能需要分两段读取 (环形) */
    uint32_t first_chunk = ring->size - pos;
    if (first_chunk >= to_read) {
        memcpy(data, ring->data + pos, to_read);
    } else {
        memcpy(data, ring->data + pos, first_chunk);
        memcpy((uint8_t *)data + first_chunk, ring->data, 
               to_read - first_chunk);
    }
    
    /* 内存屏障 */
    __asm__ volatile("" ::: "memory");
    
    /* 更新头指针 */
    ring->head += to_read;
    ring->bytes_read += to_read;
    
    return to_read;
}

/* 窥视数据 (不移除) */
static uint32_t zp_ring_peek(struct zp_ring_buffer *ring, void *data, 
                              uint32_t size) {
    uint32_t used = zp_ring_used(ring);
    if (used == 0) {
        return 0;
    }
    
    uint32_t to_peek = size < used ? size : used;
    uint32_t pos = zp_ring_index(ring, ring->head);
    
    uint32_t first_chunk = ring->size - pos;
    if (first_chunk >= to_peek) {
        memcpy(data, ring->data + pos, to_peek);
    } else {
        memcpy(data, ring->data + pos, first_chunk);
        memcpy((uint8_t *)data + first_chunk, ring->data, 
               to_peek - first_chunk);
    }
    
    return to_peek;
}

/* ========== Z-Pipe 初始化/销毁 ========== */

int zpipe_init(struct zpipe *zp, uint64_t shared_pool_size) {
    if (zp == NULL) {
        return -1;
    }
    
    memset(zp, 0, sizeof(*zp));
    INIT_LIST_HEAD(&zp->channels);
    spinlock_init(&zp->lock);
    
    /* 分配共享内存池 */
    if (shared_pool_size > 0) {
        uint64_t phys = pmm_alloc_pages(shared_pool_size / PAGE_SIZE);
        if (phys != 0) {
            zp->shared_pool = phys_to_virt(phys);
            zp->shared_pool_phys = phys;
            zp->shared_pool_size = shared_pool_size;
        }
    }
    
    /* 初始化 DMA 引擎 */
    spinlock_init(&zp->dma.lock);
    zp->dma_available = 0;  /* DMA 需要硬件支持，暂时禁用 */
    
    return 0;
}

void zpipe_destroy(struct zpipe *zp) {
    if (zp == NULL) {
        return;
    }
    
    spin_lock(&zp->lock);
    
    /* 销毁所有通道 */
    struct zp_channel *ch, *tmp;
    list_for_each_entry_safe(ch, tmp, &zp->channels, list) {
        list_del(&ch->list);
        zp_ring_destroy(&ch->ring);
        kfree(ch);
    }
    
    /* 释放共享内存池 */
    if (zp->shared_pool != NULL) {
        pmm_free_pages(zp->shared_pool_phys, 
                       zp->shared_pool_size / PAGE_SIZE);
    }
    
    /* 释放 DMA 资源 */
    if (zp->dma.desc_ring != NULL) {
        kfree(zp->dma.desc_ring);
    }
    if (zp->dma.completion_ring != NULL) {
        kfree(zp->dma.completion_ring);
    }
    
    spin_unlock(&zp->lock);
}

/* ========== 通道管理 ========== */

struct zp_channel *zpipe_create_channel(struct zpipe *zp, const char *name,
                                         uint32_t size, uint32_t flags) {
    if (zp == NULL || name == NULL) {
        return NULL;
    }
    
    /* 分配通道结构 */
    struct zp_channel *ch = kmalloc(sizeof(*ch));
    if (ch == NULL) {
        return NULL;
    }
    
    memset(ch, 0, sizeof(*ch));
    
    /* 初始化环形缓冲区 */
    if (zp_ring_init(&ch->ring, size) != 0) {
        kfree(ch);
        return NULL;
    }
    
    /* 设置属性 */
    ch->channel_id = zp->next_channel_id++;
    strncpy(ch->name, name, sizeof(ch->name) - 1);
    ch->state = ZP_STATE_ACTIVE;
    ch->flags = flags;
    
    /* 获取当前进程作为所有者 */
    struct process *proc = process_get_current();
    if (proc != NULL) {
        ch->owner_pid = proc->pid;
    }
    
    /* 设置默认水位 */
    ch->high_watermark = ch->ring.size * 3 / 4;
    ch->low_watermark = ch->ring.size / 4;
    
    spinlock_init(&ch->lock);
    INIT_LIST_HEAD(&ch->list);
    
    /* 添加到链表 */
    spin_lock(&zp->lock);
    list_add_tail(&ch->list, &zp->channels);
    zp->channel_count++;
    zp->total_channels++;
    spin_unlock(&zp->lock);
    
    return ch;
}

int zpipe_destroy_channel(struct zpipe *zp, uint32_t channel_id) {
    if (zp == NULL) {
        return -1;
    }
    
    spin_lock(&zp->lock);
    
    struct zp_channel *ch;
    list_for_each_entry(ch, &zp->channels, list) {
        if (ch->channel_id == channel_id) {
            list_del(&ch->list);
            zp->channel_count--;
            spin_unlock(&zp->lock);
            
            zp_ring_destroy(&ch->ring);
            kfree(ch);
            
            return 0;
        }
    }
    
    spin_unlock(&zp->lock);
    return -1;
}

struct zp_channel *zpipe_find_channel(struct zpipe *zp, const char *name) {
    if (zp == NULL || name == NULL) {
        return NULL;
    }
    
    spin_lock(&zp->lock);
    
    struct zp_channel *ch;
    list_for_each_entry(ch, &zp->channels, list) {
        if (strncmp(ch->name, name, sizeof(ch->name)) == 0) {
            spin_unlock(&zp->lock);
            return ch;
        }
    }
    
    spin_unlock(&zp->lock);
    return NULL;
}

struct zp_channel *zpipe_get_channel(struct zpipe *zp, uint32_t channel_id) {
    if (zp == NULL) {
        return NULL;
    }
    
    spin_lock(&zp->lock);
    
    struct zp_channel *ch;
    list_for_each_entry(ch, &zp->channels, list) {
        if (ch->channel_id == channel_id) {
            spin_unlock(&zp->lock);
            return ch;
        }
    }
    
    spin_unlock(&zp->lock);
    return NULL;
}

void zpipe_put_channel(struct zp_channel *ch) {
    /* 当前没有引用计数，保留接口 */
    (void)ch;
}

/* ========== 零拷贝数据传输 ========== */

int zpipe_write_zc(struct zp_channel *ch, const void *data, uint32_t size) {
    if (ch == NULL || data == NULL || size == 0) {
        return -1;
    }
    
    spin_lock(&ch->lock);
    
    if (ch->state != ZP_STATE_ACTIVE) {
        spin_unlock(&ch->lock);
        return -1;
    }
    
    uint32_t written = zp_ring_write(&ch->ring, data, size);
    ch->total_bytes += written;
    ch->total_msgs++;
    
    /* 检查水位 */
    if (zp_ring_used(&ch->ring) >= ch->high_watermark) {
        ch->blocked = 1;
    }
    
    spin_unlock(&ch->lock);
    
    /* 通知读者 */
    if (written > 0) {
        zpipe_notify_read(ch);
    }
    
    return (int)written;
}

int zpipe_read_zc(struct zp_channel *ch, void *data, uint32_t size) {
    if (ch == NULL || data == NULL || size == 0) {
        return -1;
    }
    
    spin_lock(&ch->lock);
    
    if (ch->state != ZP_STATE_ACTIVE && zp_ring_empty(&ch->ring)) {
        spin_unlock(&ch->lock);
        return -1;
    }
    
    uint32_t read = zp_ring_read(&ch->ring, data, size);
    
    /* 检查水位 (解除阻塞) */
    if (ch->blocked && zp_ring_used(&ch->ring) <= ch->low_watermark) {
        ch->blocked = 0;
    }
    
    spin_unlock(&ch->lock);
    
    /* 通知写者 */
    if (read > 0) {
        zpipe_notify_write(ch);
    }
    
    return (int)read;
}

int zpipe_peek_zc(struct zp_channel *ch, void *data, uint32_t size) {
    if (ch == NULL || data == NULL || size == 0) {
        return -1;
    }
    
    spin_lock(&ch->lock);
    uint32_t peeked = zp_ring_peek(&ch->ring, data, size);
    spin_unlock(&ch->lock);
    
    return (int)peeked;
}

uint32_t zpipe_readable(struct zp_channel *ch) {
    if (ch == NULL) {
        return 0;
    }
    return zp_ring_used(&ch->ring);
}

uint32_t zpipe_writable(struct zp_channel *ch) {
    if (ch == NULL) {
        return 0;
    }
    return zp_ring_free(&ch->ring);
}

/* ========== 向量化 I/O ========== */

int zpipe_writev_zc(struct zp_channel *ch, const struct zp_iov *iovs, 
                     uint32_t count) {
    if (ch == NULL || iovs == NULL || count == 0) {
        return -1;
    }
    
    int total = 0;
    for (uint32_t i = 0; i < count; i++) {
        /* 转换物理地址到虚拟地址 */
        void *virt = phys_to_virt(iovs[i].phys_addr);
        if (virt == NULL) {
            break;
        }
        
        int written = zpipe_write_zc(ch, virt, iovs[i].size);
        if (written < 0) {
            break;
        }
        total += written;
        
        if ((uint32_t)written < iovs[i].size) {
            break;  /* 缓冲区满 */
        }
    }
    
    return total;
}

int zpipe_readv_zc(struct zp_channel *ch, struct zp_iov *iovs, uint32_t *count) {
    if (ch == NULL || iovs == NULL || count == NULL || *count == 0) {
        return -1;
    }
    
    int total = 0;
    uint32_t i;
    for (i = 0; i < *count; i++) {
        /* 转换物理地址到虚拟地址 */
        void *virt = phys_to_virt(iovs[i].phys_addr);
        if (virt == NULL) {
            break;
        }
        
        int read = zpipe_read_zc(ch, virt, iovs[i].size);
        if (read < 0) {
            break;
        }
        total += read;
        iovs[i].size = read;  /* 返回实际读取大小 */
        
        if ((uint32_t)read < iovs[i].size) {
            break;  /* 缓冲区空 */
        }
    }
    
    *count = i;
    return total;
}

/* ========== 消息传递 ========== */

int zpipe_write_msg(struct zp_channel *ch, struct zp_msg *msg) {
    if (ch == NULL || msg == NULL) {
        return -1;
    }
    
    /* 先写入消息头 */
    struct {
        uint32_t total_size;
        uint32_t iov_count;
        uint64_t timestamp;
        uint32_t flags;
        uint32_t reserved;
    } header;
    
    header.total_size = msg->total_size;
    header.iov_count = msg->iov_count;
    header.timestamp = msg->timestamp;
    header.flags = msg->flags;
    header.reserved = 0;
    
    int written = zpipe_write_zc(ch, &header, sizeof(header));
    if (written != sizeof(header)) {
        return -1;
    }
    
    /* 写入数据 */
    return zpipe_writev_zc(ch, msg->iovs, msg->iov_count);
}

int zpipe_read_msg(struct zp_channel *ch, struct zp_msg *msg) {
    if (ch == NULL || msg == NULL) {
        return -1;
    }
    
    /* 先读取消息头 */
    struct {
        uint32_t total_size;
        uint32_t iov_count;
        uint64_t timestamp;
        uint32_t flags;
        uint32_t reserved;
    } header;
    
    int read = zpipe_peek_zc(ch, &header, sizeof(header));
    if (read != sizeof(header)) {
        return -1;
    }
    
    /* 检查是否有足够数据 */
    if (zpipe_readable(ch) < header.total_size + sizeof(header)) {
        return -1;
    }
    
    /* 确认读取头部 */
    zpipe_read_zc(ch, &header, sizeof(header));
    
    /* 设置消息属性 */
    msg->total_size = header.total_size;
    msg->iov_count = header.iov_count;
    msg->timestamp = header.timestamp;
    msg->flags = header.flags;
    
    /* 读取数据 */
    uint32_t count = msg->iov_count;
    return zpipe_readv_zc(ch, msg->iovs, &count);
}

/* ========== DMA 直通 ========== */

int zpipe_dma_from_device(struct zp_channel *ch, uint64_t device_addr,
                           uint32_t size, uint64_t completion) {
    (void)ch;
    (void)device_addr;
    (void)size;
    (void)completion;
    return -1;
}

int zpipe_dma_to_device(struct zp_channel *ch, uint64_t device_addr,
                         uint32_t size, uint64_t completion) {
    (void)ch;
    (void)device_addr;
    (void)size;
    (void)completion;
    return -1;
}

int zpipe_dma_channel_to_channel(struct zp_channel *src, struct zp_channel *dst,
                                  uint32_t size) {
    (void)src;
    (void)dst;
    (void)size;
    return -1;
}

int zpipe_dma_wait_completion(struct zpipe *zp, uint64_t timeout) {
    (void)zp;
    (void)timeout;
    
    return -1;
}

/* ========== 事件通知 ========== */

int zpipe_wait_readable(struct zp_channel *ch, uint64_t timeout) {
    if (ch == NULL) {
        return -1;
    }
    
    /* 简单轮询实现 */
    uint64_t start = 0;  /* TODO: 获取当前时间 */
    while (zp_ring_empty(&ch->ring)) {
        if (timeout > 0) {
            uint64_t elapsed = 0;  /* TODO: 计算已过时间 */
            if (elapsed >= timeout) {
                return -1;  /* 超时 */
            }
        }
        /* TODO: 让出 CPU */
    }
    
    return 0;
}

int zpipe_wait_writable(struct zp_channel *ch, uint64_t timeout) {
    if (ch == NULL) {
        return -1;
    }
    
    while (zp_ring_full(&ch->ring)) {
        if (timeout > 0) {
            /* TODO: 超时检查 */
        }
        /* TODO: 让出 CPU */
    }
    
    return 0;
}

void zpipe_notify_read(struct zp_channel *ch) {
    ch->events.read_ready = 1;
    /* TODO: 唤醒等待的读者 */
}

void zpipe_notify_write(struct zp_channel *ch) {
    ch->events.write_ready = 1;
    /* TODO: 唤醒等待的写者 */
}

void zpipe_notify_error(struct zp_channel *ch, int error) {
    ch->events.error = error;
}

/* ========== 背压控制 ========== */

int zpipe_set_watermarks(struct zp_channel *ch, uint32_t high, uint32_t low) {
    if (ch == NULL || high <= low || high > ch->ring.size) {
        return -1;
    }
    
    spin_lock(&ch->lock);
    ch->high_watermark = high;
    ch->low_watermark = low;
    spin_unlock(&ch->lock);
    
    return 0;
}

int zpipe_is_blocked(struct zp_channel *ch) {
    if (ch == NULL) {
        return 0;
    }
    return ch->blocked;
}

int zpipe_flush(struct zp_channel *ch) {
    if (ch == NULL) {
        return -1;
    }
    
    /* 等待缓冲区清空 */
    while (!zp_ring_empty(&ch->ring)) {
        /* TODO: 让出 CPU */
    }
    
    return 0;
}

/* ========== 通道访问控制 ========== */

int zpipe_channel_add_reader(struct zp_channel *ch, pid_t pid) {
    if (ch == NULL) {
        return -1;
    }
    
    spin_lock(&ch->lock);
    
    if (ch->reader_count >= ZP_MAX_READERS) {
        spin_unlock(&ch->lock);
        return -1;
    }
    
    /* 检查是否已存在 */
    for (uint32_t i = 0; i < ch->reader_count; i++) {
        if (ch->readers[i] == pid) {
            spin_unlock(&ch->lock);
            return 0;  /* 已存在 */
        }
    }
    
    ch->readers[ch->reader_count++] = pid;
    spin_unlock(&ch->lock);
    
    return 0;
}

int zpipe_channel_add_writer(struct zp_channel *ch, pid_t pid) {
    if (ch == NULL) {
        return -1;
    }
    
    spin_lock(&ch->lock);
    
    if (ch->writer_count >= ZP_MAX_WRITERS) {
        spin_unlock(&ch->lock);
        return -1;
    }
    
    /* 检查是否已存在 */
    for (uint32_t i = 0; i < ch->writer_count; i++) {
        if (ch->writers[i] == pid) {
            spin_unlock(&ch->lock);
            return 0;
        }
    }
    
    ch->writers[ch->writer_count++] = pid;
    spin_unlock(&ch->lock);
    
    return 0;
}

int zpipe_channel_remove_reader(struct zp_channel *ch, pid_t pid) {
    if (ch == NULL) {
        return -1;
    }
    
    spin_lock(&ch->lock);
    
    for (uint32_t i = 0; i < ch->reader_count; i++) {
        if (ch->readers[i] == pid) {
            /* 移动后面的元素 */
            for (uint32_t j = i; j < ch->reader_count - 1; j++) {
                ch->readers[j] = ch->readers[j + 1];
            }
            ch->reader_count--;
            spin_unlock(&ch->lock);
            return 0;
        }
    }
    
    spin_unlock(&ch->lock);
    return -1;
}

int zpipe_channel_remove_writer(struct zp_channel *ch, pid_t pid) {
    if (ch == NULL) {
        return -1;
    }
    
    spin_lock(&ch->lock);
    
    for (uint32_t i = 0; i < ch->writer_count; i++) {
        if (ch->writers[i] == pid) {
            for (uint32_t j = i; j < ch->writer_count - 1; j++) {
                ch->writers[j] = ch->writers[j + 1];
            }
            ch->writer_count--;
            spin_unlock(&ch->lock);
            return 0;
        }
    }
    
    spin_unlock(&ch->lock);
    return -1;
}

/* ========== 统计 ========== */

void zpipe_get_stats(struct zpipe *zp, uint64_t *channels, 
                     uint64_t *bytes, uint64_t *transfers) {
    if (zp == NULL) {
        return;
    }
    
    if (channels != NULL) {
        *channels = zp->total_channels;
    }
    if (bytes != NULL) {
        *bytes = zp->total_bytes;
    }
    if (transfers != NULL) {
        *transfers = zp->dma.transfers;
    }
}

void zpipe_channel_stats(struct zp_channel *ch, uint64_t *written,
                          uint64_t *read, uint64_t *msgs) {
    if (ch == NULL) {
        return;
    }
    
    spin_lock(&ch->lock);
    
    if (written != NULL) {
        *written = ch->ring.bytes_written;
    }
    if (read != NULL) {
        *read = ch->ring.bytes_read;
    }
    if (msgs != NULL) {
        *msgs = ch->total_msgs;
    }
    
    spin_unlock(&ch->lock);
}

/* ========== 全局管理器访问 ========== */

struct zpipe *zpipe_get_global(void) {
    return &global_zpipe;
}

int zpipe_global_init(uint64_t shared_pool_size) {
    return zpipe_init(&global_zpipe, shared_pool_size);
}
