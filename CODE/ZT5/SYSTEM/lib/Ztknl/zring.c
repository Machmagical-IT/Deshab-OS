/*
 * Zratro OS - Z-Ring Implementation
 * 
 * 原创异步 I/O 系统实现
 */

#include "zring.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"
#include "vfs.h"

/* ========== 缓冲池实现 ========== */

static int buffer_pool_init(struct zr_buffer_pool *pool, uint32_t count) {
    INIT_LIST_HEAD(&pool->free_list);
    INIT_LIST_HEAD(&pool->used_list);
    pool->total = 0;
    pool->free_count = 0;
    pool->lock = SPINLOCK_INIT;
    (void)count;
    return 0;
}

static void buffer_pool_destroy(struct zr_buffer_pool *pool) {
    spin_lock(&pool->lock);
    
    struct zr_buffer *buf, *next;
    list_for_each_entry_safe(buf, next, &pool->free_list, list) {
        list_del(&buf->list);
        kfree(buf);
    }
    list_for_each_entry_safe(buf, next, &pool->used_list, list) {
        list_del(&buf->list);
        kfree(buf);
    }
    
    spin_unlock(&pool->lock);
}

/* ========== 优先级队列实现 ========== */

static void priority_queue_init(struct zr_priority_queue *pq) {
    for (int i = 0; i < ZR_MAX_PRIORITY; i++) {
        INIT_LIST_HEAD(&pq->queues[i]);
        pq->counts[i] = 0;
    }
    pq->lock = SPINLOCK_INIT;
}

static void priority_queue_push(struct zr_priority_queue *pq, struct zr_event *event) {
    int prio = event->priority;
    if (prio >= ZR_MAX_PRIORITY) prio = ZR_MAX_PRIORITY - 1;
    
    spin_lock(&pq->lock);
    list_add_tail(&event->list, &pq->queues[prio]);
    pq->counts[prio]++;
    spin_unlock(&pq->lock);
}

static struct zr_event *priority_queue_pop(struct zr_priority_queue *pq) {
    spin_lock(&pq->lock);
    
    for (int i = 0; i < ZR_MAX_PRIORITY; i++) {
        if (!list_empty(&pq->queues[i])) {
            struct zr_event *event = list_first_entry(&pq->queues[i], struct zr_event, list);
            list_del(&event->list);
            pq->counts[i]--;
            spin_unlock(&pq->lock);
            return event;
        }
    }
    
    spin_unlock(&pq->lock);
    return NULL;
}

/* ========== 回调链实现 ========== */

static void callback_chain_init(struct zr_callback_chain *chain) {
    chain->head = NULL;
    chain->tail = NULL;
    chain->count = 0;
    chain->lock = SPINLOCK_INIT;
}

static void callback_chain_destroy(struct zr_callback_chain *chain) {
    spin_lock(&chain->lock);
    
    struct zr_callback *cb = chain->head;
    while (cb != NULL) {
        struct zr_callback *next = cb->next;
        kfree(cb);
        cb = next;
    }
    
    chain->head = NULL;
    chain->tail = NULL;
    chain->count = 0;
    
    spin_unlock(&chain->lock);
}

static void callback_chain_invoke(struct zr_callback_chain *chain, struct zr_cqe *cqe) {
    spin_lock(&chain->lock);
    
    struct zr_callback *cb = chain->head;
    while (cb != NULL) {
        if (cb->fn != NULL) {
            cb->fn(cqe, cb->data);
        }
        cb = cb->next;
    }
    
    spin_unlock(&chain->lock);
}

/* ========== 主 API 实现 ========== */

int zring_init(struct zring *zr, uint32_t ring_size, uint32_t buf_count) {
    if (ring_size == 0) ring_size = ZR_RING_SIZE;
    
    /* 清零所有指针，便于错误处理 */
    zr->ring.sq_head = NULL;
    zr->ring.sq_tail = NULL;
    zr->ring.cq_head = NULL;
    zr->ring.cq_tail = NULL;
    zr->ring.flags = NULL;
    zr->ring.features = NULL;
    zr->ring.sqes = NULL;
    zr->ring.cqes = NULL;
    zr->files = NULL;
    
    /* 分配环形缓冲区 */
    zr->ring.sq_head = kmalloc(sizeof(uint32_t));
    if (!zr->ring.sq_head) goto cleanup;
    
    zr->ring.sq_tail = kmalloc(sizeof(uint32_t));
    if (!zr->ring.sq_tail) goto cleanup;
    
    zr->ring.cq_head = kmalloc(sizeof(uint32_t));
    if (!zr->ring.cq_head) goto cleanup;
    
    zr->ring.cq_tail = kmalloc(sizeof(uint32_t));
    if (!zr->ring.cq_tail) goto cleanup;
    
    zr->ring.flags = kmalloc(sizeof(uint32_t));
    if (!zr->ring.flags) goto cleanup;
    
    zr->ring.features = kmalloc(sizeof(uint32_t));
    if (!zr->ring.features) goto cleanup;
    
    *zr->ring.sq_head = 0;
    *zr->ring.sq_tail = 0;
    *zr->ring.cq_head = 0;
    *zr->ring.cq_tail = 0;
    *zr->ring.flags = ZR_FLAG_ENABLED;
    *zr->ring.features = 0;
    
    /* 分配 SQ 和 CQ 条目数组 */
    zr->ring.sq_size = ring_size;
    zr->ring.sq_mask = ring_size - 1;
    zr->ring.cq_size = ring_size * 2;  /* CQ 更大 */
    zr->ring.cq_mask = (ring_size * 2) - 1;
    
    zr->ring.sqes = kmalloc(sizeof(struct zr_sqe) * ring_size);
    if (!zr->ring.sqes) goto cleanup;
    
    zr->ring.cqes = kmalloc(sizeof(struct zr_cqe) * ring_size * 2);
    if (!zr->ring.cqes) goto cleanup;
    
    /* 清零 */
    for (uint32_t i = 0; i < ring_size; i++) {
        zr->ring.sqes[i].op = 0;
        zr->ring.sqes[i].flags = 0;
        zr->ring.sqes[i].user_data = 0;
    }
    
    for (uint32_t i = 0; i < ring_size * 2; i++) {
        zr->ring.cqes[i].user_data = 0;
        zr->ring.cqes[i].result = 0;
        zr->ring.cqes[i].flags = 0;
    }
    
    zr->ring.submitted = 0;
    zr->ring.completed = 0;
    zr->ring.errors = 0;
    
    /* 初始化缓冲池 */
    buffer_pool_init(&zr->buf_pool, buf_count);
    
    /* 初始化批处理器 */
    zr->batch.count = 0;
    zr->batch.optimal_size = 32;
    zr->batch.adaptive_threshold = 1000;
    
    /* 初始化优先级队列 */
    priority_queue_init(&zr->pq);
    
    /* 初始化回调链 */
    callback_chain_init(&zr->cb_chain);
    
    /* 文件描述符表 */
    zr->file_capacity = 256;
    zr->file_count = 0;
    zr->files = kmalloc(sizeof(void *) * zr->file_capacity);
    if (zr->files == NULL) goto cleanup;
    
    for (uint32_t i = 0; i < zr->file_capacity; i++) {
        zr->files[i] = NULL;
    }
    
    zr->worker_count = 1;
    zr->running = 1;
    
    zr->total_requests = 0;
    zr->total_bytes = 0;
    zr->avg_latency = 0;
    
    zr->lock = SPINLOCK_INIT;
    
    println("[ZRING] Initialized");
    return 0;
    
cleanup:
    /* 分配失败时的清理 */
    kfree(zr->ring.sq_head);
    kfree(zr->ring.sq_tail);
    kfree(zr->ring.cq_head);
    kfree(zr->ring.cq_tail);
    kfree(zr->ring.flags);
    kfree(zr->ring.features);
    kfree(zr->ring.sqes);
    kfree(zr->ring.cqes);
    kfree(zr->files);
    println("[ZRING] Error: Memory allocation failed");
    return -1;
}

void zring_destroy(struct zring *zr) {
    zr->running = 0;
    
    kfree(zr->ring.sq_head);
    kfree(zr->ring.sq_tail);
    kfree(zr->ring.cq_head);
    kfree(zr->ring.cq_tail);
    kfree(zr->ring.flags);
    kfree(zr->ring.features);
    kfree(zr->ring.sqes);
    kfree(zr->ring.cqes);
    kfree(zr->files);
    
    buffer_pool_destroy(&zr->buf_pool);
    callback_chain_destroy(&zr->cb_chain);
}

/* 获取空闲 SQE */
struct zr_sqe *zring_get_sqe(struct zring *zr) {
    spin_lock(&zr->lock);
    
    uint32_t head = *zr->ring.sq_head;
    uint32_t tail = *zr->ring.sq_tail;
    
    /* 检查是否已满 */
    if ((tail - head) >= zr->ring.sq_size) {
        spin_unlock(&zr->lock);
        return NULL;
    }
    
    struct zr_sqe *sqe = &zr->ring.sqes[tail & zr->ring.sq_mask];
    
    /* 清零 */
    sqe->op = 0;
    sqe->flags = 0;
    sqe->prio = 0;
    sqe->fd = 0;
    sqe->user_data = 0;
    sqe->link_next = 0;
    sqe->link_prev = 0;
    sqe->depends_on = 0;
    sqe->rw.addr = 0;
    sqe->rw.len = 0;
    sqe->rw.off = 0;
    
    (*zr->ring.sq_tail)++;
    
    spin_unlock(&zr->lock);
    return sqe;
}

/* 提交请求 */
int zring_submit(struct zring *zr, struct zr_sqe *sqe) {
    if (zr == NULL || sqe == NULL) return -1;
    
    /* SQE 已经在队列中，只需通知处理 */
    zr->ring.submitted++;
    zr->total_requests++;
    
    return 0;
}

/* 批量提交 */
int zring_submit_batch(struct zring *zr, struct zr_sqe *sqes, uint32_t count) {
    if (zr == NULL || sqes == NULL || count == 0) return -1;
    
    for (uint32_t i = 0; i < count; i++) {
        struct zr_sqe *sqe = zring_get_sqe(zr);
        if (sqe == NULL) break;
        
        /* 复制请求 */
        sqe->op = sqes[i].op;
        sqe->flags = sqes[i].flags;
        sqe->prio = sqes[i].prio;
        sqe->fd = sqes[i].fd;
        sqe->user_data = sqes[i].user_data;
        sqe->rw.addr = sqes[i].rw.addr;
        sqe->rw.len = sqes[i].rw.len;
        sqe->rw.off = sqes[i].rw.off;
        
        zr->ring.submitted++;
        zr->total_requests++;
    }
    
    return 0;
}

/* 收取完成事件 */
int zring_peek_cqe(struct zring *zr, struct zr_cqe **cqe) {
    if (zr == NULL || cqe == NULL) return -1;
    
    uint32_t head = *zr->ring.cq_head;
    uint32_t tail = *zr->ring.cq_tail;
    
    if (head == tail) {
        return -1;  /* 无完成事件 */
    }
    
    *cqe = &zr->ring.cqes[head & zr->ring.cq_mask];
    return 0;
}

int zring_wait_cqe(struct zring *zr, struct zr_cqe **cqe, uint64_t timeout) {
    if (zr == NULL || cqe == NULL) return -1;
    
    uint64_t start = get_tick_count();
    uint64_t end = start + timeout;
    
    while (get_tick_count() < end) {
        if (zring_peek_cqe(zr, cqe) == 0) {
            return 0;
        }
        /* 让出 CPU */
        asm volatile("pause");
    }
    
    return -1;  /* 超时 */
}

int zring_seen_cqe(struct zring *zr, struct zr_cqe *cqe) {
    if (zr == NULL || cqe == NULL) return -1;
    
    spin_lock(&zr->lock);
    (*zr->ring.cq_head)++;
    spin_unlock(&zr->lock);
    
    return 0;
}

/* 批量收取 */
uint32_t zring_batch_cqe(struct zring *zr, struct zr_cqe **cqes, uint32_t count) {
    if (zr == NULL || cqes == NULL) return 0;
    
    uint32_t i = 0;
    for (; i < count; i++) {
        if (zring_peek_cqe(zr, &cqes[i]) != 0) {
            break;
        }
        zring_seen_cqe(zr, cqes[i]);
    }
    
    return i;
}

/* ========== 请求预构建 ========== */

void zring_prep_read(struct zr_sqe *sqe, int fd, void *buf, size_t len, off_t off) {
    sqe->op = ZR_OP_READ;
    sqe->fd = fd;
    sqe->rw.addr = (uint64_t)buf;
    sqe->rw.len = len;
    sqe->rw.off = off;
}

void zring_prep_write(struct zr_sqe *sqe, int fd, const void *buf, size_t len, off_t off) {
    sqe->op = ZR_OP_WRITE;
    sqe->fd = fd;
    sqe->rw.addr = (uint64_t)buf;
    sqe->rw.len = len;
    sqe->rw.off = off;
}

void zring_prep_readv(struct zr_sqe *sqe, int fd, const struct zr_iovec *iov, int iov_cnt, off_t off) {
    sqe->op = ZR_OP_READV;
    sqe->fd = fd;
    sqe->rwv.iovec_addr = (uint64_t)iov;
    sqe->rwv.iov_cnt = iov_cnt;
    sqe->rwv.off = off;
}

void zring_prep_writev(struct zr_sqe *sqe, int fd, const struct zr_iovec *iov, int iov_cnt, off_t off) {
    sqe->op = ZR_OP_WRITEV;
    sqe->fd = fd;
    sqe->rwv.iovec_addr = (uint64_t)iov;
    sqe->rwv.iov_cnt = iov_cnt;
    sqe->rwv.off = off;
}

void zring_prep_poll(struct zr_sqe *sqe, int fd, uint32_t events) {
    sqe->op = ZR_OP_POLL;
    sqe->fd = fd;
    sqe->poll.events = events;
    sqe->poll.timeout = 0;
}

void zring_prep_accept(struct zr_sqe *sqe, int fd, void *addr, uint32_t *addrlen) {
    sqe->op = ZR_OP_ACCEPT;
    sqe->fd = fd;
    sqe->sockaddr.addr = (uint64_t)addr;
    sqe->sockaddr.len = (uint32_t)(uint64_t)addrlen;
}

void zring_prep_connect(struct zr_sqe *sqe, int fd, const void *addr, uint32_t addrlen) {
    sqe->op = ZR_OP_CONNECT;
    sqe->fd = fd;
    sqe->sockaddr.addr = (uint64_t)addr;
    sqe->sockaddr.len = addrlen;
}

void zring_prep_send(struct zr_sqe *sqe, int fd, const void *buf, size_t len) {
    sqe->op = ZR_OP_SEND;
    sqe->fd = fd;
    sqe->rw.addr = (uint64_t)buf;
    sqe->rw.len = len;
}

void zring_prep_recv(struct zr_sqe *sqe, int fd, void *buf, size_t len) {
    sqe->op = ZR_OP_RECV;
    sqe->fd = fd;
    sqe->rw.addr = (uint64_t)buf;
    sqe->rw.len = len;
}

void zring_prep_sendfile(struct zr_sqe *sqe, int out_fd, int in_fd, size_t count, off_t *off) {
    sqe->op = ZR_OP_SENDFILE;
    sqe->fd = out_fd;
    sqe->splice.in_fd = in_fd;
    sqe->splice.out_fd = out_fd;
    sqe->splice.len = count;
    sqe->splice.off_in = off ? *off : 0;
    sqe->splice.off_out = 0;
}

void zring_prep_fsync(struct zr_sqe *sqe, int fd) {
    sqe->op = ZR_OP_FSYNC;
    sqe->fd = fd;
}

void zring_prep_timeout(struct zr_sqe *sqe, uint64_t nsec) {
    sqe->op = ZR_OP_TIMEOUT;
    sqe->poll.timeout = nsec;
}

/* ========== 处理循环 ========== */

/* 处理单个请求 */
static int process_sqe(struct zring *zr, struct zr_sqe *sqe, struct zr_cqe *cqe) {
    int64_t result = 0;
    
    switch (sqe->op) {
    case ZR_OP_READ: {
        /* 实际读取操作 */
        void *buf = (void *)sqe->rw.addr;
        size_t len = sqe->rw.len;
        off_t off = sqe->rw.off;
        
        /* 调用 VFS 读取 */
        /* struct file *file = zr->files[sqe->fd]; */
        /* result = vfs_read(file, buf, len, off); */
        
        /* 简化实现 */
        result = len;
        zr->total_bytes += len;
        break;
    }
    
    case ZR_OP_WRITE: {
        void *buf = (void *)sqe->rw.addr;
        size_t len = sqe->rw.len;
        off_t off = sqe->rw.off;
        
        /* result = vfs_write(file, buf, len, off); */
        result = len;
        zr->total_bytes += len;
        break;
    }
    
    case ZR_OP_READV: {
        struct zr_iovec *iov = (struct zr_iovec *)sqe->rwv.iovec_addr;
        int iov_cnt = sqe->rwv.iov_cnt;
        
        size_t total = 0;
        for (int i = 0; i < iov_cnt; i++) {
            total += iov[i].iov_len;
        }
        result = total;
        zr->total_bytes += total;
        break;
    }
    
    case ZR_OP_WRITEV: {
        struct zr_iovec *iov = (struct zr_iovec *)sqe->rwv.iovec_addr;
        int iov_cnt = sqe->rwv.iov_cnt;
        
        size_t total = 0;
        for (int i = 0; i < iov_cnt; i++) {
            total += iov[i].iov_len;
        }
        result = total;
        zr->total_bytes += total;
        break;
    }
    
    case ZR_OP_FSYNC:
        /* 同步操作 */
        result = 0;
        break;
    
    case ZR_OP_POLL:
        /* 轮询操作 */
        result = sqe->poll.events;
        break;
    
    case ZR_OP_TIMEOUT:
        /* 超时操作 */
        result = 0;
        break;
    
    default:
        result = -1;
        break;
    }
    
    /* 填充完成事件 */
    cqe->user_data = sqe->user_data;
    cqe->result = result;
    cqe->flags = ZR_CQE_FLAG_DONE;
    cqe->error = 0;
    cqe->timestamp = get_tick_count();
    
    return 0;
}

int zring_process(struct zring *zr) {
    if (zr == NULL || !zr->running) return -1;
    
    struct zr_sqe *sqe;
    struct zr_cqe cqe;
    int processed = 0;
    
    /* 处理提交队列 */
    while (*zr->ring.sq_head != *zr->ring.sq_tail) {
        sqe = &zr->ring.sqes[(*zr->ring.sq_head) & zr->ring.sq_mask];
        
        if (process_sqe(zr, sqe, &cqe) == 0) {
            /* 放入完成队列 */
            uint32_t tail = *zr->ring.cq_tail;
            zr->ring.cqes[tail & zr->ring.cq_mask] = cqe;
            (*zr->ring.cq_tail)++;
            
            zr->ring.completed++;
            processed++;
        }
        
        (*zr->ring.sq_head)++;
    }
    
    /* 调用回调链 */
    if (processed > 0) {
        callback_chain_invoke(&zr->cb_chain, &cqe);
    }
    
    return processed;
}

int zring_process_batch(struct zring *zr, uint32_t count) {
    int total = 0;
    
    for (uint32_t i = 0; i < count; i++) {
        int processed = zring_process(zr);
        if (processed <= 0) break;
        total += processed;
    }
    
    return total;
}

int zring_flush(struct zring *zr) {
    int total = 0;
    
    while (zr_ring_pending(&zr->ring) > 0) {
        int processed = zring_process(zr);
        if (processed <= 0) break;
        total += processed;
    }
    
    return total;
}

/* ========== 统计 ========== */

void zring_get_stats(struct zring *zr, uint64_t *requests, 
                     uint64_t *bytes, uint64_t *latency) {
    if (requests) *requests = zr->total_requests;
    if (bytes) *bytes = zr->total_bytes;
    if (latency) *latency = zr->avg_latency;
}

/* ========== 高级特性 ========== */

/* 自适应批处理调优 */
void zring_adaptive_batch_tune(struct zring *zr, uint64_t avg_latency) {
    /* 根据延迟调整批次大小 */
    if (avg_latency > zr->batch.adaptive_threshold * 2) {
        /* 延迟过高，减小批次 */
        if (zr->batch.optimal_size > 8) {
            zr->batch.optimal_size /= 2;
        }
    } else if (avg_latency < zr->batch.adaptive_threshold / 2) {
        /* 延迟较低，增大批次 */
        if (zr->batch.optimal_size < 64) {
            zr->batch.optimal_size *= 2;
        }
    }
    
    zr->avg_latency = avg_latency;
}

/* 设置优先级 */
void zring_set_priority(struct zr_sqe *sqe, uint16_t prio) {
    sqe->prio = prio;
}

/* 取消请求 */
int zring_cancel(struct zring *zr, uint64_t user_data) {
    spin_lock(&zr->lock);
    
    /* 遍历提交队列查找 */
    uint32_t head = *zr->ring.sq_head;
    uint32_t tail = *zr->ring.sq_tail;
    
    for (uint32_t i = head; i < tail; i++) {
        struct zr_sqe *sqe = &zr->ring.sqes[i & zr->ring.sq_mask];
        if (sqe->user_data == user_data) {
            /* 标记为已取消 */
            sqe->op = ZR_OP_CANCEL;
            spin_unlock(&zr->lock);
            return 0;
        }
    }
    
    spin_unlock(&zr->lock);
    return -1;
}

/* 取消所有请求 */
int zring_cancel_all(struct zring *zr) {
    spin_lock(&zr->lock);
    
    *zr->ring.sq_head = *zr->ring.sq_tail;
    
    spin_unlock(&zr->lock);
    return 0;
}

/* 零拷贝发送 */
int zring_zero_copy_send(struct zring *zr, int out_fd, int in_fd,
                         size_t count, off_t *off_in, off_t *off_out) {
    struct zr_sqe *sqe = zring_get_sqe(zr);
    if (sqe == NULL) return -1;
    
    zring_prep_sendfile(sqe, out_fd, in_fd, count, off_in);
    if (off_out) sqe->splice.off_out = *off_out;
    
    return zring_submit(zr, sqe);
}

/* 注册缓冲区 */
int zring_register_buffer(struct zring *zr, void *buf, size_t len) {
    struct zr_buffer *buffer = kmalloc(sizeof(struct zr_buffer));
    if (buffer == NULL) return -1;
    
    buffer->addr = (uint64_t)buf;
    buffer->len = len;
    buffer->ref_count = 1;
    
    spin_lock(&zr->buf_pool.lock);
    buffer->index = zr->buf_pool.total++;
    list_add(&buffer->list, &zr->buf_pool.used_list);
    spin_unlock(&zr->buf_pool.lock);
    
    return buffer->index;
}

int zring_unregister_buffer(struct zring *zr, uint32_t index) {
    spin_lock(&zr->buf_pool.lock);
    
    struct zr_buffer *buf;
    list_for_each_entry(buf, &zr->buf_pool.used_list, list) {
        if (buf->index == index) {
            list_del(&buf->list);
            zr->buf_pool.free_count++;
            spin_unlock(&zr->buf_pool.lock);
            kfree(buf);
            return 0;
        }
    }
    
    spin_unlock(&zr->buf_pool.lock);
    return -1;
}

struct zr_buffer *zring_get_buffer(struct zring *zr, uint32_t index) {
    spin_lock(&zr->buf_pool.lock);
    
    struct zr_buffer *buf;
    list_for_each_entry(buf, &zr->buf_pool.used_list, list) {
        if (buf->index == index) {
            spin_unlock(&zr->buf_pool.lock);
            return buf;
        }
    }
    
    spin_unlock(&zr->buf_pool.lock);
    return NULL;
}

/* 文件注册 */
int zring_register_file(struct zring *zr, int fd, uint32_t *index) {
    spin_lock(&zr->lock);
    
    if (zr->file_count >= zr->file_capacity) {
        /* 扩容 */
        uint32_t new_cap = zr->file_capacity * 2;
        void **new_files = kmalloc(sizeof(void *) * new_cap);
        if (new_files == NULL) {
            spin_unlock(&zr->lock);
            return -1;
        }
        
        for (uint32_t i = 0; i < zr->file_count; i++) {
            new_files[i] = zr->files[i];
        }
        
        kfree(zr->files);
        zr->files = new_files;
        zr->file_capacity = new_cap;
    }
    
    uint32_t idx = zr->file_count++;
    zr->files[idx] = (void *)(uint64_t)fd;
    
    if (index) *index = idx;
    
    spin_unlock(&zr->lock);
    return 0;
}

int zring_unregister_file(struct zring *zr, uint32_t index) {
    spin_lock(&zr->lock);
    
    if (index < zr->file_count) {
        zr->files[index] = NULL;
        spin_unlock(&zr->lock);
        return 0;
    }
    
    spin_unlock(&zr->lock);
    return -1;
}

/* 回调注册 */
int zring_set_callback(struct zring *zr, uint64_t user_data, zr_callback_fn fn, void *data) {
    struct zr_callback *cb = kmalloc(sizeof(struct zr_callback));
    if (cb == NULL) return -1;
    
    cb->fn = fn;
    cb->data = data;
    cb->next = NULL;
    
    spin_lock(&zr->cb_chain.lock);
    
    /* 替换原有回调 */
    struct zr_callback *old = zr->cb_chain.head;
    while (old != NULL) {
        struct zr_callback *next = old->next;
        kfree(old);
        old = next;
    }
    
    zr->cb_chain.head = cb;
    zr->cb_chain.tail = cb;
    zr->cb_chain.count = 1;
    
    spin_unlock(&zr->cb_chain.lock);
    
    (void)user_data;
    return 0;
}

int zring_add_callback(struct zring *zr, uint64_t user_data, zr_callback_fn fn, void *data) {
    struct zr_callback *cb = kmalloc(sizeof(struct zr_callback));
    if (cb == NULL) return -1;
    
    cb->fn = fn;
    cb->data = data;
    cb->next = NULL;
    
    spin_lock(&zr->cb_chain.lock);
    
    if (zr->cb_chain.tail != NULL) {
        zr->cb_chain.tail->next = cb;
    }
    zr->cb_chain.tail = cb;
    
    if (zr->cb_chain.head == NULL) {
        zr->cb_chain.head = cb;
    }
    
    zr->cb_chain.count++;
    
    spin_unlock(&zr->cb_chain.lock);
    
    (void)user_data;
    return 0;
}

/* 链式操作 */
int zring_link_sqes(struct zring *zr, struct zr_sqe *sqe1, struct zr_sqe *sqe2) {
    if (sqe1 == NULL || sqe2 == NULL) return -1;
    
    /* 设置链接标志 */
    sqe1->flags |= ZR_SQE_FLAG_IO_LINK;
    
    /* 记录链接关系 */
    uint32_t idx1 = sqe1 - zr->ring.sqes;
    uint32_t idx2 = sqe2 - zr->ring.sqes;
    
    sqe1->link_next = idx2;
    sqe2->link_prev = idx1;
    
    return 0;
}

/* ========== IO 优先级管理 ========== */

/* IO 优先级类 */
#define ZR_IO_CLASS_RT       0   /* 实时 */
#define ZR_IO_CLASS_BE       1   /* 最佳努力 */
#define ZR_IO_CLASS_IDLE     2   /* 空闲 */

/* 优先级队列调度统计 */
struct zr_prio_stats {
    uint64_t dispatched[ZR_MAX_PRIORITY];
    uint64_t wait_time_total[ZR_MAX_PRIORITY];
    uint64_t wait_time_max[ZR_MAX_PRIORITY];
};

static struct zr_prio_stats prio_stats;

/* 设置 IO 优先级类 */
int zring_set_ioprio_class(struct zr_sqe *sqe, int class) {
    if (sqe == NULL) return -1;
    
    switch (class) {
    case ZR_IO_CLASS_RT:
        sqe->prio = 0;  /* 最高优先级 */
        break;
    case ZR_IO_CLASS_BE:
        sqe->prio = ZR_MAX_PRIORITY / 2;
        break;
    case ZR_IO_CLASS_IDLE:
        sqe->prio = ZR_MAX_PRIORITY - 1;
        break;
    default:
        return -1;
    }
    
    return 0;
}

/* 设置 IO 优先级值 (在类内部) */
int zring_set_ioprio_value(struct zr_sqe *sqe, int value) {
    if (sqe == NULL || value < 0 || value >= ZR_MAX_PRIORITY) {
        return -1;
    }
    
    sqe->prio = value;
    return 0;
}

/* 提交时记录等待时间 */
static uint64_t sqe_submit_time[ZR_RING_SIZE];

/* 调度下一个 SQE (基于优先级) */
struct zr_sqe *zring_schedule_next(struct zring *zr) {
    /* 按优先级遍历 */
    for (int p = 0; p < ZR_MAX_PRIORITY; p++) {
        if (!list_empty(&zr->pq.queues[p])) {
            struct zr_event *event = list_first_entry(&zr->pq.queues[p], 
                                                       struct zr_event, list);
            list_del(&event->list);
            zr->pq.counts[p]--;
            
            /* 更新统计 */
            prio_stats.dispatched[p]++;
            
            return (struct zr_sqe *)event;
        }
    }
    
    return NULL;
}

/* ========== 超时管理 ========== */

/* 超时请求结构 */
struct zr_timeout {
    uint64_t user_data;
    uint64_t deadline;
    uint32_t flags;
    struct list_head list;
};

#define ZR_TIMEOUT_FLAG_ABSOLUTE    (1 << 0)   /* 绝对时间 */
#define ZR_TIMEOUT_FLAG_CANCELED    (1 << 1)   /* 已取消 */

/* 超时队列 */
static struct list_head timeout_list = LIST_HEAD_INIT(timeout_list);
static spinlock_t timeout_lock = SPINLOCK_INIT;

/* 提交超时请求 */
int zring_submit_timeout(struct zring *zr, uint64_t user_data, 
                         uint64_t nsec, int absolute) {
    struct zr_timeout *tmo = kmalloc(sizeof(struct zr_timeout));
    if (tmo == NULL) return -1;
    
    tmo->user_data = user_data;
    tmo->flags = absolute ? ZR_TIMEOUT_FLAG_ABSOLUTE : 0;
    
    uint64_t now = get_tick_count();
    tmo->deadline = absolute ? nsec : (now + nsec);
    
    spin_lock(&timeout_lock);
    
    /* 按截止时间排序插入 */
    struct zr_timeout *pos;
    int inserted = 0;
    list_for_each_entry(pos, &timeout_list, list) {
        if (tmo->deadline < pos->deadline) {
            list_add_tail(&tmo->list, &pos->list);
            inserted = 1;
            break;
        }
    }
    
    if (!inserted) {
        list_add_tail(&tmo->list, &timeout_list);
    }
    
    spin_unlock(&timeout_lock);
    
    return 0;
}

/* 取消超时请求 */
int zring_cancel_timeout(struct zring *zr, uint64_t user_data) {
    spin_lock(&timeout_lock);
    
    struct zr_timeout *tmo;
    list_for_each_entry(tmo, &timeout_list, list) {
        if (tmo->user_data == user_data) {
            tmo->flags |= ZR_TIMEOUT_FLAG_CANCELED;
            spin_unlock(&timeout_lock);
            return 0;
        }
    }
    
    spin_unlock(&timeout_lock);
    return -1;
}

/* 检查超时 */
void zring_check_timeouts(struct zring *zr) {
    uint64_t now = get_tick_count();
    
    spin_lock(&timeout_lock);
    
    struct zr_timeout *tmo, *next;
    list_for_each_entry_safe(tmo, next, &timeout_list, list) {
        if (tmo->flags & ZR_TIMEOUT_FLAG_CANCELED) {
            list_del(&tmo->list);
            kfree(tmo);
            continue;
        }
        
        if (tmo->deadline <= now) {
            /* 超时触发，生成完成事件 */
            struct zr_cqe cqe;
            cqe.user_data = tmo->user_data;
            cqe.result = -1;  /* 超时 */
            cqe.flags = ZR_CQE_FLAG_TIMEOUT;
            cqe.error = 1;
            
            /* 放入完成队列 */
            uint32_t tail = *zr->ring.cq_tail;
            zr->ring.cqes[tail & zr->ring.cq_mask] = cqe;
            (*zr->ring.cq_tail)++;
            
            list_del(&tmo->list);
            kfree(tmo);
        } else {
            /* 后面的还未超时 */
            break;
        }
    }
    
    spin_unlock(&timeout_lock);
}

/* 设置请求截止时间 */
int zring_set_deadline(struct zr_sqe *sqe, uint64_t deadline) {
    if (sqe == NULL) return -1;
    sqe->deadline = deadline;
    sqe->flags |= ZR_SQE_FLAG_DEADLINE;
    return 0;
}

/* ========== IO 事件通知 ========== */

/* 事件通知结构 */
struct zr_notify {
    uint64_t user_data;
    uint32_t events;        /* 监听的事件 */
    uint32_t triggered;     /* 已触发的事件 */
    void (*callback)(uint64_t user_data, uint32_t events);
    struct list_head list;
};

/* 事件通知列表 */
static struct list_head notify_list = LIST_HEAD_INIT(notify_list);
static spinlock_t notify_lock = SPINLOCK_INIT;

/* 注册事件通知 */
int zring_register_notify(struct zring *zr, uint64_t user_data, 
                          uint32_t events, void (*callback)(uint64_t, uint32_t)) {
    struct zr_notify *notif = kmalloc(sizeof(struct zr_notify));
    if (notif == NULL) return -1;
    
    notif->user_data = user_data;
    notif->events = events;
    notif->triggered = 0;
    notif->callback = callback;
    
    spin_lock(&notify_lock);
    list_add(&notif->list, &notify_list);
    spin_unlock(&notify_lock);
    
    return 0;
}

/* 取消事件通知 */
int zring_cancel_notify(struct zring *zr, uint64_t user_data) {
    spin_lock(&notify_lock);
    
    struct zr_notify *notif;
    list_for_each_entry(notif, &notify_list, list) {
        if (notif->user_data == user_data) {
            list_del(&notif->list);
            kfree(notif);
            spin_unlock(&notify_lock);
            return 0;
        }
    }
    
    spin_unlock(&notify_lock);
    return -1;
}

/* 触发事件 */
void zring_trigger_event(struct zring *zr, uint32_t event_type) {
    spin_lock(&notify_lock);
    
    struct zr_notify *notif;
    list_for_each_entry(notif, &notify_list, list) {
        if (notif->events & event_type) {
            notif->triggered |= event_type;
            
            if (notif->callback != NULL) {
                notif->callback(notif->user_data, event_type);
            }
        }
    }
    
    spin_unlock(&notify_lock);
}

/* ========== 异步 IO 池 ========== */

/* IO 工作线程 */
struct zr_worker {
    uint32_t id;
    struct zring *zr;
    void *stack;
    uint32_t flags;
    uint64_t processed;
    void *stat;                  /* 统计数据指针 */
};

#define ZR_WORKER_FLAG_RUNNING   (1 << 0)
#define ZR_WORKER_FLAG_ACTIVE    (1 << 1)

/* 工作线程池 */
static struct zr_worker workers[4];
static uint32_t worker_count = 0;

/* 创建工作线程 */
int zring_create_worker(struct zring *zr) {
    if (worker_count >= 4) return -1;
    
    struct zr_worker *worker = &workers[worker_count];
    worker->id = worker_count;
    worker->zr = zr;
    worker->flags = ZR_WORKER_FLAG_RUNNING;
    worker->processed = 0;
    
    /* 分配栈 */
    worker->stack = kmalloc(0x10000);  /* 64KB 栈 */
    if (worker->stack == NULL) {
        return -1;
    }
    
    worker_count++;
    
    return worker->id;
}

/* 销毁工作线程 */
void zring_destroy_worker(uint32_t id) {
    if (id >= worker_count) return;
    
    struct zr_worker *worker = &workers[id];
    worker->flags &= ~ZR_WORKER_FLAG_RUNNING;
    
    if (worker->stack != NULL) {
        kfree(worker->stack);
        worker->stat = NULL;
    }
}

/* 工作线程主循环 */
void zr_worker_loop(struct zr_worker *worker) {
    while (worker->flags & ZR_WORKER_FLAG_RUNNING) {
        int processed = zring_process(worker->zr);
        if (processed > 0) {
            worker->processed += processed;
            worker->flags |= ZR_WORKER_FLAG_ACTIVE;
        } else {
            worker->flags &= ~ZR_WORKER_FLAG_ACTIVE;
            /* 等待新请求 */
            asm volatile("pause");
        }
        
        /* 检查超时 */
        zring_check_timeouts(worker->zr);
    }
}

/* ========== 带优先级的批量提交 ========== */

/* 按优先级批量提交 */
int zring_submit_batch_prio(struct zring *zr, struct zr_sqe *sqes, 
                            uint32_t count, uint16_t *priorities) {
    if (zr == NULL || sqes == NULL || count == 0) return -1;
    
    /* 按优先级分组 */
    for (uint32_t i = 0; i < count; i++) {
        struct zr_sqe *sqe = zring_get_sqe(zr);
        if (sqe == NULL) break;
        
        /* 复制请求 */
        sqe->op = sqes[i].op;
        sqe->flags = sqes[i].flags;
        sqe->prio = priorities ? priorities[i] : sqes[i].prio;
        sqe->fd = sqes[i].fd;
        sqe->user_data = sqes[i].user_data;
        sqe->rw = sqes[i].rw;
        
        /* 加入优先级队列 */
        struct zr_event *event = (struct zr_event *)sqe;
        event->priority = sqe->prio;
        priority_queue_push(&zr->pq, event);
        
        zr->ring.submitted++;
        zr->total_requests++;
    }
    
    return 0;
}

/* ========== IO 调度器 ========== */

/* 调度策略 */
#define ZR_SCHED_FIFO       0   /* 先进先出 */
#define ZR_SCHED_PRIO       1   /* 优先级调度 */
#define ZR_SCHED_DEADLINE   2   /* 截止时间调度 */
#define ZR_SCHED_FAIR       3   /* 公平调度 */

/* 设置调度策略 */
int zring_set_scheduler(struct zring *zr, int policy) {
    if (policy < ZR_SCHED_FIFO || policy > ZR_SCHED_FAIR) {
        return -1;
    }
    
    zr->scheduler_policy = policy;
    return 0;
}

/* 公平调度权重 */
static uint64_t prio_weights[ZR_MAX_PRIORITY] = {
    1024, 896, 768, 640, 512, 384, 256, 128
};

/* 公平调度选择 */
static struct zr_sqe *fair_schedule(struct zring *zr) {
    /* 计算虚拟运行时间 */
    uint64_t min_vruntime = 0xFFFFFFFFFFFFFFFFULL;
    struct zr_sqe *selected = NULL;
    
    /* 遍历所有优先级队列 */
    for (int p = 0; p < ZR_MAX_PRIORITY; p++) {
        if (list_empty(&zr->pq.queues[p])) continue;
        
        struct zr_event *event = list_first_entry(&zr->pq.queues[p], 
                                                   struct zr_event, list);
        struct zr_sqe *sqe = (struct zr_sqe *)event;
        
        /* 计算虚拟运行时间 */
        uint64_t vruntime = sqe->runtime / prio_weights[p];
        
        if (vruntime < min_vruntime) {
            min_vruntime = vruntime;
            selected = sqe;
        }
    }
    
    return selected;
}

/* 截止时间调度选择 */
static struct zr_sqe *deadline_schedule(struct zring *zr) {
    uint64_t now = get_tick_count();
    uint64_t earliest_deadline = 0xFFFFFFFFFFFFFFFFULL;
    struct zr_sqe *selected = NULL;
    
    for (int p = 0; p < ZR_MAX_PRIORITY; p++) {
        if (list_empty(&zr->pq.queues[p])) continue;
        
        struct zr_event *event = list_first_entry(&zr->pq.queues[p], 
                                                   struct zr_event, list);
        struct zr_sqe *sqe = (struct zr_sqe *)event;
        
        uint64_t deadline = (sqe->flags & ZR_SQE_FLAG_DEADLINE) ? 
                            sqe->deadline : now + 10000000000ULL;
        
        if (deadline < earliest_deadline) {
            earliest_deadline = deadline;
            selected = sqe;
        }
    }
    
    return selected;
}

/* 统一调度入口 */
struct zr_sqe *zring_dispatch(struct zring *zr) {
    switch (zr->scheduler_policy) {
    case ZR_SCHED_PRIO:
        return zring_schedule_next(zr);
    case ZR_SCHED_DEADLINE:
        return deadline_schedule(zr);
    case ZR_SCHED_FAIR:
        return fair_schedule(zr);
    case ZR_SCHED_FIFO:
    default:
        /* FIFO: 直接从 SQ 取 */
        if (*zr->ring.sq_head != *zr->ring.sq_tail) {
            return &zr->ring.sqes[(*zr->ring.sq_head) & zr->ring.sq_mask];
        }
        return NULL;
    }
}

/* ========== 性能监控 ========== */

/* IO 延迟统计 */
struct zr_latency_stats {
    uint64_t min;
    uint64_t max;
    uint64_t total;
    uint64_t count;
    uint64_t percentiles[5];  /* 50%, 75%, 90%, 95%, 99% */
};

static struct zr_latency_stats latency_stats;

/* 更新延迟统计 */
void zr_update_latency(uint64_t latency) {
    if (latency < latency_stats.min || latency_stats.count == 0) {
        latency_stats.min = latency;
    }
    if (latency > latency_stats.max) {
        latency_stats.max = latency;
    }
    latency_stats.total += latency;
    latency_stats.count++;
    
    /* 简化的百分位数估计 */
    if (latency_stats.count > 100) {
        latency_stats.percentiles[0] = latency_stats.total / latency_stats.count;
    }
}

/* 获取延迟统计 */
void zring_get_latency_stats(struct zr_latency_stats *stats) {
    *stats = latency_stats;
}

/* 重置统计 */
void zring_reset_stats(struct zring *zr) {
    zr->total_requests = 0;
    zr->total_bytes = 0;
    zr->avg_latency = 0;
    zr->ring.submitted = 0;
    zr->ring.completed = 0;
    zr->ring.errors = 0;
    
    memset(&latency_stats, 0, sizeof(latency_stats));
    memset(&prio_stats, 0, sizeof(prio_stats));
}

/* ========== 高级 API ========== */

/* 异步读取带回调 */
int zring_read_async(struct zring *zr, int fd, void *buf, size_t len,
                     off_t off, zr_callback_fn callback, void *data) {
    struct zr_sqe *sqe = zring_get_sqe(zr);
    if (sqe == NULL) return -1;
    
    zring_prep_read(sqe, fd, buf, len, off);
    
    uint64_t user_data = zr->total_requests;
    sqe->user_data = user_data;
    sqe->submit_time = get_tick_count();
    
    zring_set_callback(zr, user_data, callback, data);
    
    return zring_submit(zr, sqe);
}

/* 异步写入带回调 */
int zring_write_async(struct zring *zr, int fd, const void *buf, size_t len,
                      off_t off, zr_callback_fn callback, void *data) {
    struct zr_sqe *sqe = zring_get_sqe(zr);
    if (sqe == NULL) return -1;
    
    zring_prep_write(sqe, fd, buf, len, off);
    
    uint64_t user_data = zr->total_requests;
    sqe->user_data = user_data;
    sqe->submit_time = get_tick_count();
    
    zring_set_callback(zr, user_data, callback, data);
    
    return zring_submit(zr, sqe);
}

/* 带超时的读取 */
int zring_read_timeout(struct zring *zr, int fd, void *buf, size_t len,
                       off_t off, uint64_t timeout_ns) {
    struct zr_sqe *sqe = zring_get_sqe(zr);
    if (sqe == NULL) return -1;
    
    zring_prep_read(sqe, fd, buf, len, off);
    zring_set_deadline(sqe, get_tick_count() + timeout_ns);
    
    sqe->user_data = zr->total_requests;
    
    return zring_submit(zr, sqe);
}

/* 带重试的写入 */
int zring_write_retry(struct zring *zr, int fd, const void *buf, size_t len,
                      off_t off, uint32_t max_retries) {
    struct zr_sqe *sqe = zring_get_sqe(zr);
    if (sqe == NULL) return -1;
    
    zring_prep_write(sqe, fd, buf, len, off);
    sqe->flags |= ZR_SQE_FLAG_RETRY;
    sqe->retry_count = max_retries;
    sqe->user_data = zr->total_requests;
    
    return zring_submit(zr, sqe);
}

/* 批量读取向量 */
int zring_readv_batch(struct zring *zr, int fd, struct zr_iovec *iovs, 
                      int iov_cnt, off_t *offs, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        struct zr_sqe *sqe = zring_get_sqe(zr);
        if (sqe == NULL) break;
        
        zring_prep_readv(sqe, fd, &iovs[i], iov_cnt, offs ? offs[i] : 0);
        sqe->user_data = zr->total_requests + i;
    }
    
    return 0;
}

/* ========== Z-Ring 高级增强功能 ========== */

/* === 超时处理增强 === */

/* 取消请求 */
int zring_cancel_request(struct zring *zr, uint64_t user_data) {
    spin_lock(&zr->lock);
    
    /* 遍历 SQ 查找请求 */
    uint32_t head = *zr->ring.sq_head;
    uint32_t tail = *zr->ring.sq_tail;
    
    while (head != tail) {
        struct zr_sqe *sqe = &zr->ring.sqes[head & zr->ring.sq_mask];
        
        if (sqe->user_data == user_data) {
            sqe->flags |= ZR_CQE_FLAG_CANCELLED;
            spin_unlock(&zr->lock);
            return 0;
        }
        
        head++;
    }
    
    spin_unlock(&zr->lock);
    return -1;
}

/* === 统计增强 === */

/* 获取详细统计 */
void zring_get_full_stats(struct zring *zr, uint64_t *requests, uint64_t *bytes,
                          uint64_t *submitted, uint64_t *completed, uint64_t *errors) {
    if (requests) *requests = zr->total_requests;
    if (bytes) *bytes = zr->total_bytes;
    if (submitted) *submitted = zr->ring.submitted;
    if (completed) *completed = zr->ring.completed;
    if (errors) *errors = zr->ring.errors;
}

/* === 环回测试 === */

/* 自测函数 */
int zring_selftest(struct zring *zr) {
    int passed = 0;
    int failed = 0;
    
    println("[ZRING] Starting self-test...");
    
    /* 测试 SQE 获取 */
    struct zr_sqe *sqe = zring_get_sqe(zr);
    if (sqe != NULL) {
        passed++;
    } else {
        failed++;
        println("  FAIL: SQE allocation");
    }
    
    /* 测试提交 */
    if (sqe != NULL) {
        zring_prep_poll(sqe, 0, 0x01);  /* POLLIN */
        sqe->user_data = 0xDEADBEEF;
        
        if (zring_submit(zr, sqe) == 0) {
            passed++;
        } else {
            failed++;
            println("  FAIL: SQE submission");
        }
    }
    
    /* 测试等待 */
    struct zr_cqe *cqe = NULL;
    if (zring_wait_cqe(zr, &cqe, 1000000000ULL) == 0 && cqe != NULL) {
        passed++;
        zring_seen_cqe(zr, cqe);
    } else {
        failed++;
        println("  FAIL: CQE wait");
    }
    
    print("[ZRING] Test result: ");
    print_dec(passed);
    print(" passed, ");
    print_dec(failed);
    println(" failed");
    
    return failed == 0 ? 0 : -1;
}

/* === 完整诊断报告 === */

void zring_print_report(struct zring *zr) {
    if (zr == NULL) return;
    
    println("╔══════════════════════════════════════════╗");
    println("║        Z-Ring Diagnostic Report          ║");
    println("╚══════════════════════════════════════════╝");
    
    println("");
    println("=== 环形缓冲区状态 ===");
    print("SQ 大小: "); print_dec(zr->ring.sq_size); println("");
    print("CQ 大小: "); print_dec(zr->ring.cq_size); println("");
    print("SQ 头: "); print_dec(*zr->ring.sq_head); println("");
    print("SQ 尾: "); print_dec(*zr->ring.sq_tail); println("");
    print("CQ 头: "); print_dec(*zr->ring.cq_head); println("");
    print("CQ 尾: "); print_dec(*zr->ring.cq_tail); println("");
    
    println("");
    println("=== 操作统计 ===");
    print("总请求数: "); print_dec(zr->total_requests); println("");
    print("总字节数: "); print_dec(zr->total_bytes); println("");
    print("已提交: "); print_dec(zr->ring.submitted); println("");
    print("已完成: "); print_dec(zr->ring.completed); println("");
    print("错误数: "); print_dec(zr->ring.errors); println("");
    print("平均延迟: "); print_dec(zr->avg_latency / 1000000); println(" ms");
    
    println("");
    println("=== 缓冲池状态 ===");
    print("总缓冲区: "); print_dec(zr->buf_pool.total); println("");
    print("空闲缓冲区: "); print_dec(zr->buf_pool.free_count); println("");
    
    println("");
    println("=== 延迟统计 ===");
    struct zr_latency_stats lat;
    zring_get_latency_stats(&lat);
    print("最小延迟: "); print_dec(lat.min / 1000); println(" us");
    print("最大延迟: "); print_dec(lat.max / 1000); println(" us");
    print("平均延迟: "); print_dec(lat.total / (lat.count > 0 ? lat.count : 1) / 1000); 
    println(" us");
}

/* ========== Zratro 原创增强: 零拷贝 API 实现 ========== */

/* 零拷贝发送 (使用注册缓冲区) */
void zring_prep_send_zc(struct zr_sqe *sqe, int fd, uint32_t buf_idx, 
                        uint32_t buf_off, uint32_t len) {
    if (sqe == NULL) return;
    
    sqe->op = ZR_OP_SEND_ZC;
    sqe->fd = fd;
    sqe->zc.buf_idx = buf_idx;
    sqe->zc.buf_off = buf_off;
    sqe->zc.len = len;
    sqe->zc.flags = 0;
    sqe->flags |= ZR_SQE_FLAG_FIXED_FILE;
}

/* 零拷贝接收 (使用注册缓冲区) */
void zring_prep_recv_zc(struct zr_sqe *sqe, int fd, uint32_t buf_idx,
                        uint32_t buf_off, uint32_t len) {
    if (sqe == NULL) return;
    
    sqe->op = ZR_OP_RECV_ZC;
    sqe->fd = fd;
    sqe->zc.buf_idx = buf_idx;
    sqe->zc.buf_off = buf_off;
    sqe->zc.len = len;
    sqe->zc.flags = 0;
    sqe->flags |= ZR_SQE_FLAG_FIXED_FILE;
}

/* 提供缓冲区给内核 */
void zring_prep_provide_buffers(struct zr_sqe *sqe, void *addr, 
                                 uint32_t len, uint32_t count, uint32_t bgid) {
    if (sqe == NULL) return;
    
    sqe->op = ZR_OP_PROVIDE_BUFFERS;
    sqe->provide_buf.addr = (uint64_t)addr;
    sqe->provide_buf.len = len;
    sqe->provide_buf.count = count;
    sqe->provide_buf.bgid = bgid;
}

/* 移除缓冲区 */
void zring_prep_remove_buffers(struct zr_sqe *sqe, void *addr, 
                                uint32_t count, uint32_t bgid) {
    if (sqe == NULL) return;
    
    sqe->op = ZR_OP_REMOVE_BUFFERS;
    sqe->provide_buf.addr = (uint64_t)addr;
    sqe->provide_buf.count = count;
    sqe->provide_buf.bgid = bgid;
}

/* 注册持久缓冲区 (用于零拷贝) */
int zring_register_persistent_buffer(struct zring *zr, void *buf, 
                                      size_t len, uint32_t *idx) {
    if (zr == NULL || buf == NULL || len == 0) return -1;
    
    struct zr_buffer *buffer = kmalloc(sizeof(struct zr_buffer));
    if (buffer == NULL) return -1;
    
    buffer->addr = (uint64_t)buf;
    buffer->len = len;
    buffer->ref_count = 1;
    
    spin_lock(&zr->buf_pool.lock);
    
    buffer->index = zr->buf_pool.total++;
    list_add(&buffer->list, &zr->buf_pool.used_list);
    
    spin_unlock(&zr->buf_pool.lock);
    
    if (idx != NULL) {
        *idx = buffer->index;
    }
    
    return 0;
}

/* 获取缓冲区物理地址 (DMA 用) */
uint64_t zring_get_buffer_phys(struct zring *zr, uint32_t idx) {
    if (zr == NULL) return 0;
    
    spin_lock(&zr->buf_pool.lock);
    
    struct zr_buffer *buf;
    list_for_each_entry(buf, &zr->buf_pool.used_list, list) {
        if (buf->index == idx) {
            /* Higher Half 内核需要转换虚拟地址到物理地址 */
            uint64_t vaddr = buf->addr;
            uint64_t paddr;
            
            /* 检查是否为 Higher Half 地址 */
            if (vaddr >= 0xffffffff80000000ULL) {
                /* 减去内核基址偏移 */
                extern uint64_t hhdm_offset;
                paddr = vaddr - 0xffffffff80000000ULL;
            } else {
                /* 可能已经通过 HHDM 映射 */
                extern uint64_t hhdm_offset;
                paddr = vaddr - hhdm_offset;
            }
            
            spin_unlock(&zr->buf_pool.lock);
            return paddr;
        }
    }
    
    spin_unlock(&zr->buf_pool.lock);
    return 0;
}

/* 零拷贝 sendfile (DMA 直通) */
int zring_sendfile_dma(struct zring *zr, int out_fd, int in_fd,
                       size_t count, off_t *off_in) {
    if (zr == NULL) return -1;
    
    struct zr_sqe *sqe = zring_get_sqe(zr);
    if (sqe == NULL) return -1;
    
    sqe->op = ZR_OP_SENDFILE;
    sqe->fd = out_fd;
    sqe->splice.in_fd = in_fd;
    sqe->splice.out_fd = out_fd;
    sqe->splice.len = count;
    sqe->splice.off_in = off_in ? *off_in : 0;
    sqe->splice.off_out = 0;
    sqe->flags |= ZR_SQE_FLAG_FIXED_FILE;
    sqe->user_data = zr->total_requests;
    
    /* 标记为零拷贝模式 */
    sqe->flags |= 0x80;  /* 自定义零拷贝标志 */
    
    return zring_submit(zr, sqe);
}

/* 处理零拷贝操作 */
static int process_zc_op(struct zring *zr, struct zr_sqe *sqe, struct zr_cqe *cqe) {
    int64_t result = 0;
    
    switch (sqe->op) {
    case ZR_OP_SEND_ZC: {
        /* 获取注册的缓冲区 */
        struct zr_buffer *buf = zring_get_buffer(zr, sqe->zc.buf_idx);
        if (buf == NULL) {
            result = -1;
            break;
        }
        
        /* 计算发送地址 */
        void *send_addr = (void *)(buf->addr + sqe->zc.buf_off);
        size_t send_len = sqe->zc.len;
        
        /* 调用网络发送 (零拷贝) */
        /* TODO: 集成网络栈零拷贝发送 */
        result = send_len;
        zr->total_bytes += send_len;
        break;
    }
    
    case ZR_OP_RECV_ZC: {
        /* 获取注册的缓冲区 */
        struct zr_buffer *buf = zring_get_buffer(zr, sqe->zc.buf_idx);
        if (buf == NULL) {
            result = -1;
            break;
        }
        
        /* 计算接收地址 */
        void *recv_addr = (void *)(buf->addr + sqe->zc.buf_off);
        size_t recv_len = sqe->zc.len;
        
        /* 调用网络接收 (零拷贝) */
        /* TODO: 集成网络栈零拷贝接收 */
        result = recv_len;
        zr->total_bytes += recv_len;
        break;
    }
    
    case ZR_OP_PROVIDE_BUFFERS: {
        /* 提供缓冲区 */
        void *addr = (void *)sqe->provide_buf.addr;
        uint32_t len = sqe->provide_buf.len;
        uint32_t count = sqe->provide_buf.count;
        
        for (uint32_t i = 0; i < count; i++) {
            uint32_t idx;
            zring_register_persistent_buffer(zr, (char *)addr + i * len, len, &idx);
        }
        
        result = count;
        break;
    }
    
    case ZR_OP_REMOVE_BUFFERS: {
        /* 移除缓冲区 */
        result = 0;
        break;
    }
    
    default:
        result = -1;
        break;
    }
    
    /* 填充完成事件 */
    cqe->user_data = sqe->user_data;
    cqe->result = result;
    cqe->flags = ZR_CQE_FLAG_DONE;
    cqe->error = 0;
    cqe->timestamp = get_tick_count();
    
    return 0;
}
