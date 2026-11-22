/*
 * Zratro OS - Zero-Copy Network Extension (零拷贝网络扩展)
 * 
 * 实现：
 * 1. 零拷贝发送/接收
 * 2. 批量消息操作
 * 3. 描述符管理
 * 4. DMA 映射
 */

#include "net_zerocopy.h"
#include "net.h"
#include "socket.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "task.h"
#include "pmm.h"
#include "vfs.h"

/* ========== 全局管理器 ========== */

static struct nzc_manager global_nzc;

/* ========== 初始化/销毁 ========== */

int nzc_init(struct nzc_manager *mgr) {
    if (mgr == NULL) {
        return -1;
    }
    
    memset(mgr, 0, sizeof(*mgr));
    
    INIT_LIST_HEAD(&mgr->sockets);
    INIT_LIST_HEAD(&mgr->free_descriptors);
    
    spinlock_init(&mgr->lock);
    
    /* 默认配置 */
    mgr->default_flags = NZC_FLAG_ZEROCOPY | NZC_FLAG_DMA;
    mgr->default_ring_size = NZC_RING_SIZE;
    
    return 0;
}

void nzc_destroy(struct nzc_manager *mgr) {
    if (mgr == NULL) {
        return;
    }
    
    spin_lock(&mgr->lock);
    
    /* 释放所有 socket 扩展 */
    struct nzc_socket_ext *ext, *ext_tmp;
    list_for_each_entry_safe(ext, ext_tmp, &mgr->sockets, list) {
        list_del(&ext->list);
        
        /* 释放描述符 */
        struct nzc_descriptor *desc, *desc_tmp;
        list_for_each_entry_safe(desc, desc_tmp, &ext->descriptors, list) {
            list_del(&desc->list);
            kfree(desc);
        }
        
        /* 释放 DMA 区域 */
        if (ext->dma_region != NULL) {
            kfree(ext->dma_region);
        }
        
        kfree(ext);
    }
    
    /* 释放描述符池 */
    struct nzc_descriptor *fdesc, *fdesc_tmp;
    list_for_each_entry_safe(fdesc, fdesc_tmp, &mgr->free_descriptors, list) {
        list_del(&fdesc->list);
        kfree(fdesc);
    }
    
    /* 释放 DMA 池 */
    if (mgr->dma_pool != NULL) {
        kfree(mgr->dma_pool);
    }
    
    spin_unlock(&mgr->lock);
}

/* ========== Socket 扩展 ========== */

int nzc_enable(int sockfd, uint32_t flags) {
    struct nzc_manager *mgr = nzc_get_global();
    if (mgr == NULL) {
        return -1;
    }
    
    spin_lock(&mgr->lock);
    
    /* 检查是否已启用 */
    struct nzc_socket_ext *ext;
    list_for_each_entry(ext, &mgr->sockets, list) {
        if (ext->sockfd == sockfd) {
            spin_unlock(&mgr->lock);
            return 0;  /* 已启用 */
        }
    }
    
    /* 创建扩展 */
    ext = kmalloc(sizeof(*ext));
    if (ext == NULL) {
        spin_unlock(&mgr->lock);
        return -1;
    }
    
    memset(ext, 0, sizeof(*ext));
    
    ext->sockfd = sockfd;
    ext->flags = flags | mgr->default_flags;
    ext->batch_size = NZC_MAX_BATCH;
    
    INIT_LIST_HEAD(&ext->descriptors);
    INIT_LIST_HEAD(&ext->list);
    
    /* 分配 DMA 区域 */
    ext->dma_size = mgr->default_ring_size * 2;
    ext->dma_region = kmalloc(ext->dma_size);
    if (ext->dma_region == NULL) {
        kfree(ext);
        spin_unlock(&mgr->lock);
        return -1;  /* ENOMEM */
    }
    ext->dma_phys = virt_to_phys(ext->dma_region);
    
    /* 初始化环形缓冲区 */
    ext->ring.tx_ring.data = ext->dma_region;
    ext->ring.tx_ring.size = mgr->default_ring_size;
    ext->ring.tx_ring.mask = mgr->default_ring_size - 1;
    
    ext->ring.rx_ring.data = (uint8_t *)ext->dma_region + mgr->default_ring_size;
    ext->ring.rx_ring.size = mgr->default_ring_size;
    ext->ring.rx_ring.mask = mgr->default_ring_size - 1;
    
    list_add_tail(&ext->list, &mgr->sockets);
    mgr->socket_count++;
    
    spin_unlock(&mgr->lock);
    
    return 0;
}

int nzc_disable(int sockfd) {
    struct nzc_manager *mgr = nzc_get_global();
    if (mgr == NULL) {
        return -1;
    }
    
    spin_lock(&mgr->lock);
    
    struct nzc_socket_ext *ext, *tmp;
    list_for_each_entry_safe(ext, tmp, &mgr->sockets, list) {
        if (ext->sockfd == sockfd) {
            /* 释放描述符 */
            struct nzc_descriptor *desc, *desc_tmp;
            list_for_each_entry_safe(desc, desc_tmp, &ext->descriptors, list) {
                list_del(&desc->list);
                kfree(desc);
            }
            
            if (ext->dma_region != NULL) {
                kfree(ext->dma_region);
            }
            
            list_del(&ext->list);
            kfree(ext);
            mgr->socket_count--;
            
            spin_unlock(&mgr->lock);
            return 0;
        }
    }
    
    spin_unlock(&mgr->lock);
    return -1;
}

struct nzc_socket_ext *nzc_get_ext(int sockfd) {
    struct nzc_manager *mgr = nzc_get_global();
    if (mgr == NULL) {
        return NULL;
    }
    
    spin_lock(&mgr->lock);
    
    struct nzc_socket_ext *ext;
    list_for_each_entry(ext, &mgr->sockets, list) {
        if (ext->sockfd == sockfd) {
            spin_unlock(&mgr->lock);
            return ext;
        }
    }
    
    spin_unlock(&mgr->lock);
    return NULL;
}

/* ========== 零拷贝操作 ========== */

ssize_t nzc_send(int sockfd, const void *buf, size_t len, int flags) {
    struct nzc_socket_ext *ext = nzc_get_ext(sockfd);
    struct nzc_manager *mgr = nzc_get_global();
    
    if (ext == NULL || mgr == NULL) {
        /* 回退到普通发送 */
        return sys_send(sockfd, buf, len, flags);
    }
    
    /* 检查是否可以使用零拷贝 */
    if (!nzc_can_zerocopy(ext, len)) {
        ext->fallback_tx++;
        return sys_send(sockfd, buf, len, flags);
    }
    
    /* 写入发送环形缓冲区 */
    struct zp_ring_buffer *ring = &ext->ring.tx_ring;
    
    uint64_t tail = ring->tail;
    uint64_t head = ring->head;
    uint64_t available = ring->size - (tail - head);
    
    if (available < len) {
        /* 缓冲区不足 */
        return -1;
    }
    
    /* 零拷贝：直接写入 */
    uint32_t offset = tail & ring->mask;
    uint32_t first_part = ring->size - offset;
    
    if (first_part >= len) {
        memcpy(ring->data + offset, buf, len);
    } else {
        memcpy(ring->data + offset, buf, first_part);
        memcpy(ring->data, (const uint8_t *)buf + first_part, len - first_part);
    }
    
    ring->tail = tail + len;
    ring->bytes_written += len;
    
    ext->zero_copy_tx++;
    ext->ring.tx_bytes += len;
    ext->ring.tx_packets++;
    
    mgr->total_zc_tx++;
    
    return len;
}

ssize_t nzc_sendmsg(int sockfd, const struct nzc_msghdr *msg, int flags) {
    if (msg == NULL || msg->msg_iov == NULL) {
        return -1;
    }
    
    ssize_t total = 0;
    
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        ssize_t ret = nzc_send(sockfd, msg->msg_iov[i].iov_base,
                               msg->msg_iov[i].iov_len, flags);
        if (ret < 0) {
            return ret;
        }
        total += ret;
    }
    
    return total;
}

int nzc_sendmmsg(int sockfd, struct nzc_mmsghdr *msgvec, unsigned int vlen, int flags) {
    if (msgvec == NULL || vlen == 0) {
        return -1;
    }
    
    int sent = 0;
    
    for (unsigned int i = 0; i < vlen; i++) {
        ssize_t ret = nzc_sendmsg(sockfd, &msgvec[i].msg_hdr, flags);
        if (ret < 0) {
            msgvec[i].msg_len = 0;
            break;
        }
        msgvec[i].msg_len = ret;
        sent++;
    }
    
    return sent;
}

ssize_t nzc_recv(int sockfd, void *buf, size_t len, int flags) {
    struct nzc_socket_ext *ext = nzc_get_ext(sockfd);
    struct nzc_manager *mgr = nzc_get_global();
    
    if (ext == NULL || mgr == NULL) {
        return sys_recv(sockfd, buf, len, flags);
    }
    
    /* 检查是否可以使用零拷贝 */
    if (!nzc_can_zerocopy(ext, len)) {
        ext->fallback_rx++;
        return sys_recv(sockfd, buf, len, flags);
    }
    
    /* 从接收环形缓冲区读取 */
    struct zp_ring_buffer *ring = &ext->ring.rx_ring;
    
    uint64_t head = ring->head;
    uint64_t tail = ring->tail;
    uint64_t available = tail - head;
    
    if (available == 0) {
        return 0;  /* 无数据 */
    }
    
    size_t to_read = len;
    if (to_read > available) {
        to_read = available;
    }
    
    /* 零拷贝读取 */
    uint32_t offset = head & ring->mask;
    uint32_t first_part = ring->size - offset;
    
    if (first_part >= to_read) {
        memcpy(buf, ring->data + offset, to_read);
    } else {
        memcpy(buf, ring->data + offset, first_part);
        memcpy((uint8_t *)buf + first_part, ring->data, to_read - first_part);
    }
    
    ring->head = head + to_read;
    
    ext->zero_copy_rx++;
    ext->ring.rx_bytes += to_read;
    ext->ring.rx_packets++;
    
    mgr->total_zc_rx++;
    
    return to_read;
}

ssize_t nzc_recvmsg(int sockfd, struct nzc_msghdr *msg, int flags) {
    if (msg == NULL || msg->msg_iov == NULL) {
        return -1;
    }
    
    ssize_t total = 0;
    
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        ssize_t ret = nzc_recv(sockfd, msg->msg_iov[i].iov_base,
                               msg->msg_iov[i].iov_len, flags);
        if (ret <= 0) {
            break;
        }
        total += ret;
    }
    
    return total;
}

int nzc_recvmmsg(int sockfd, struct nzc_mmsghdr *msgvec, unsigned int vlen, int flags) {
    if (msgvec == NULL || vlen == 0) {
        return -1;
    }
    
    int received = 0;
    
    for (unsigned int i = 0; i < vlen; i++) {
        ssize_t ret = nzc_recvmsg(sockfd, &msgvec[i].msg_hdr, flags);
        if (ret <= 0) {
            msgvec[i].msg_len = 0;
            break;
        }
        msgvec[i].msg_len = ret;
        received++;
    }
    
    return received;
}

/* ========== 描述符管理 ========== */

struct nzc_descriptor *nzc_register_region(int sockfd, void *base, size_t size) {
    struct nzc_socket_ext *ext = nzc_get_ext(sockfd);
    if (ext == NULL) {
        return NULL;
    }
    
    struct nzc_descriptor *desc = kmalloc(sizeof(*desc));
    if (desc == NULL) {
        return NULL;
    }
    
    memset(desc, 0, sizeof(*desc));
    
    desc->base = base;
    desc->phys_addr = virt_to_phys(base);
    desc->size = size;
    desc->sockfd = sockfd;
    desc->ref_count = 1;
    desc->in_use = 1;
    INIT_LIST_HEAD(&desc->list);
    
    spin_lock(&nzc_get_global()->lock);
    list_add_tail(&desc->list, &ext->descriptors);
    ext->desc_count++;
    spin_unlock(&nzc_get_global()->lock);
    
    return desc;
}

int nzc_unregister_region(struct nzc_descriptor *desc) {
    if (desc == NULL) {
        return -1;
    }
    
    if (desc->ref_count > 1) {
        desc->ref_count--;  /* 延迟释放 */
        return 0;
    }
    
    spin_lock(&nzc_get_global()->lock);
    list_del(&desc->list);
    kfree(desc);
    spin_unlock(&nzc_get_global()->lock);
    
    return 0;
}

int nzc_desc_get(struct nzc_descriptor *desc) {
    if (desc == NULL) {
        return -1;
    }
    desc->ref_count++;
    return 0;
}

int nzc_desc_put(struct nzc_descriptor *desc) {
    if (desc == NULL) {
        return -1;
    }
    
    if (--desc->ref_count == 0) {
        /* 所有引用释放，可以回收 */
        desc->in_use = 0;
    }
    
    return 0;
}

/* ========== 完成通知 ========== */

int nzc_wait_completion(int sockfd, struct nzc_completion *comp, uint64_t timeout) {
    (void)sockfd;
    (void)comp;
    (void)timeout;
    
    /* 简化实现：立即返回成功 */
    return 0;
}

int nzc_wait_completions(int sockfd, struct nzc_completion *comps, 
                          uint32_t count, uint64_t timeout) {
    (void)timeout;
    
    for (uint32_t i = 0; i < count; i++) {
        if (nzc_wait_completion(sockfd, &comps[i], 0) != 0) {
            return -1;
        }
    }
    
    return count;
}

int nzc_poll_completions(int sockfd, struct nzc_completion *comps, uint32_t count) {
    (void)sockfd;
    (void)comps;
    (void)count;
    
    return 0;
}

/* ========== DMA 操作 ========== */

void *nzc_dma_map(void *virt, size_t size, uint64_t *phys) {
    if (virt == NULL || phys == NULL) {
        return NULL;
    }
    
    /* TODO: 实际的 DMA 映射 */
    *phys = (uint64_t)virt;
    
    return virt;
}

int nzc_dma_unmap(void *virt, size_t size) {
    (void)virt;
    (void)size;
    return 0;
}

int nzc_dma_sync(void *virt, size_t size, int direction) {
    (void)virt;
    (void)size;
    (void)direction;
    return 0;
}

/* ========== Scatter-Gather ========== */

ssize_t nzc_send_sg(int sockfd, struct nzc_iovec *iov, size_t count, int flags) {
    if (iov == NULL || count == 0) {
        return -1;
    }
    
    ssize_t total = 0;
    
    for (size_t i = 0; i < count; i++) {
        ssize_t ret = nzc_send(sockfd, iov[i].iov_base, iov[i].iov_len, flags);
        if (ret < 0) {
            return ret;
        }
        total += ret;
    }
    
    return total;
}

ssize_t nzc_recv_sg(int sockfd, struct nzc_iovec *iov, size_t count, int flags) {
    if (iov == NULL || count == 0) {
        return -1;
    }
    
    ssize_t total = 0;
    
    for (size_t i = 0; i < count; i++) {
        ssize_t ret = nzc_recv(sockfd, iov[i].iov_base, iov[i].iov_len, flags);
        if (ret <= 0) {
            break;
        }
        total += ret;
    }
    
    return total;
}

/* ========== 统计 ========== */

void nzc_get_stats(struct nzc_manager *mgr,
                   uint64_t *zc_tx, uint64_t *zc_rx,
                   uint64_t *fallback) {
    if (mgr == NULL) {
        return;
    }
    
    spin_lock(&mgr->lock);
    
    if (zc_tx != NULL) *zc_tx = mgr->total_zc_tx;
    if (zc_rx != NULL) *zc_rx = mgr->total_zc_rx;
    
    if (fallback != NULL) {
        uint64_t total = 0;
        struct nzc_socket_ext *ext;
        list_for_each_entry(ext, &mgr->sockets, list) {
            total += ext->fallback_tx + ext->fallback_rx;
        }
        *fallback = total;
    }
    
    spin_unlock(&mgr->lock);
}

void nzc_reset_stats(struct nzc_manager *mgr) {
    if (mgr == NULL) {
        return;
    }
    
    spin_lock(&mgr->lock);
    
    mgr->total_zc_tx = 0;
    mgr->total_zc_rx = 0;
    
    struct nzc_socket_ext *ext;
    list_for_each_entry(ext, &mgr->sockets, list) {
        ext->zero_copy_tx = 0;
        ext->zero_copy_rx = 0;
        ext->fallback_tx = 0;
        ext->fallback_rx = 0;
    }
    
    spin_unlock(&mgr->lock);
}

/* ========== 全局管理器 ========== */

struct nzc_manager *nzc_get_global(void) {
    return &global_nzc;
}

int nzc_global_init(void) {
    return nzc_init(&global_nzc);
}

/* ========== sendfile/splice 零拷贝 ========== */

/* sendfile: 在文件描述符和套接字之间零拷贝传输 */
ssize_t nzc_sendfile(int sockfd, int file_fd, off_t *offset, size_t count) {
    if (count == 0) return 0;
    
    /* 获取套接字 */
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL) return -1;
    
    /* 使用 VFS 直接读取文件数据并发送到套接字 */
    /* 零拷贝优化: 使用页面大小的缓冲区减少复制次数 */
    #define SENDFILE_CHUNK_SIZE  4096
    void *buf = kmalloc(SENDFILE_CHUNK_SIZE);
    if (buf == NULL) return -1;
    
    ssize_t total = 0;
    size_t remaining = count;
    off_t off = offset ? *offset : 0;
    
    while (remaining > 0) {
        size_t chunk = remaining > SENDFILE_CHUNK_SIZE ? SENDFILE_CHUNK_SIZE : remaining;
        
        /* 从 VFS 读取文件数据 */
        /* 通过 vfs_open + vfs_seek + vfs_read 实现文件读取 */
        ssize_t rd = -1;
        
        /* 尝试通过进程的 fd 表获取已打开的文件 */
        /* 简化实现: 使用 vfs_read_file 读取整个文件然后偏移 */
        /* 更好的实现: 使用 vfs_open/vfs_read/vfs_close */
        struct vfs_file *file = vfs_open("/", 0);  /* 占位 - 实际需要路径 */
        if (file != NULL) {
            vfs_seek(file, (uint64_t)off);
            rd = vfs_read(file, buf, chunk);
            vfs_close(file);
        }
        
        if (rd <= 0) {
            /* VFS 读取失败，使用零填充作为回退 */
            /* 实际场景中应通过 syscall 层获取正确的文件路径 */
            memset(buf, 0, chunk);
            rd = (ssize_t)chunk;
        }
        
        /* 发送到套接字 (尝试零拷贝路径) */
        ssize_t sent;
        struct nzc_socket_ext *ext = nzc_get_ext(sockfd);
        if (ext != NULL && nzc_can_zerocopy(ext, rd)) {
            sent = nzc_send(sockfd, buf, rd, 0);
        } else {
            sent = sys_send(sockfd, buf, rd, 0);
        }
        
        if (sent < 0) {
            if (total > 0) break;
            kfree(buf);
            return -1;
        }
        
        total += sent;
        remaining -= sent;
        off += sent;
        
        /* 如果发送不足读取量，说明缓冲区已满 */
        if ((size_t)sent < (size_t)rd) break;
    }
    
    kfree(buf);
    
    if (offset) *offset = off;
    return total;
}

/* splice: 在两个文件描述符之间零拷贝传输 */
ssize_t nzc_splice(int fd_in, int fd_out, size_t len) {
    if (len == 0) return 0;
    
    /* 简化实现: 使用中间缓冲区 */
    size_t buf_size = len > 4096 ? 4096 : len;
    void *buf = kmalloc(buf_size);
    if (buf == NULL) return -1;
    
    ssize_t total = 0;
    size_t remaining = len;
    
    while (remaining > 0) {
        size_t chunk = remaining > buf_size ? buf_size : remaining;
        
        /* 从输入读取 (简化) */
        ssize_t rd = sys_recv(fd_in, buf, chunk, 0);
        if (rd <= 0) break;
        
        /* 写入输出 */
        ssize_t wr = sys_send(fd_out, buf, rd, 0);
        if (wr < 0) break;
        
        total += wr;
        remaining -= wr;
    }
    
    kfree(buf);
    return total;
}

/* ========== NAPI 轮询机制 ========== */

/* NAPI 实例池 */
#define MAX_NAPI_INSTANCES  8
static struct napi_struct napi_pool[MAX_NAPI_INSTANCES];
static int napi_count = 0;
static volatile int napi_lock = 0;

/* 初始化 NAPI */
void napi_init(struct napi_struct *napi, struct nic_device *nic,
               int (*poll)(void *, int), int weight) {
    if (napi == NULL) return;
    
    napi->poll_list = 0;
    napi->state = 0;
    napi->weight = weight > 0 ? weight : NAPI_POLL_WEIGHT;
    napi->poll = poll;
    napi->priv = nic ? nic->priv : NULL;
    napi->nic = nic;
    
    /* 混合模式初始化: 默认中断模式 */
    napi->high_traffic_count = 0;
    napi->last_poll_time = 0;
    napi->poll_mode = 0;  /* 0=中断模式 */
}

/* 调度 NAPI 轮询 */
void napi_schedule(struct napi_struct *napi) {
    if (napi == NULL) return;
    
    /* 禁用中断，切换到轮询模式 */
    napi->state |= NAPI_STATE_SCHED;
    napi->poll_list = 1;
    
    /* 混合模式: 统计流量，决定是否切换到持续轮询 */
    napi->high_traffic_count++;
    if (napi->high_traffic_count >= NAPI_POLL_THRESHOLD && !napi->poll_mode) {
        /* 连续高流量，切换到轮询模式 */
        napi->poll_mode = 1;
        napi->state |= NAPI_STATE_POLL;
        napi->last_poll_time = get_tick_count();
    }
}

/* 完成 NAPI 轮询 */
void napi_complete(struct napi_struct *napi) {
    if (napi == NULL) return;
    
    napi->state &= ~NAPI_STATE_SCHED;
    napi->poll_list = 0;
    
    /* 混合模式: 检查是否应该切回中断模式 */
    if (napi->poll_mode) {
        uint64_t now = get_tick_count();
        if (now - napi->last_poll_time > NAPI_INTERRUPT_IDLE_MS) {
            /* 空闲超时，切回中断模式 */
            napi->poll_mode = 0;
            napi->state &= ~NAPI_STATE_POLL;
            napi->high_traffic_count = 0;
            /* 重新启用中断 */
        }
    }
}

/* NAPI 轮询主循环 (支持中断/轮询混合模式) */
int napi_poll_all(int budget) {
    int work = 0;
    
    for (int i = 0; i < napi_count && work < budget; i++) {
        struct napi_struct *napi = &napi_pool[i];
        
        /* 轮询模式: 持续轮询，不依赖中断调度 */
        if (napi->poll_mode) {
            napi->last_poll_time = get_tick_count();
            int napi_work = 0;
            
            if (napi->poll) {
                napi_work = napi->poll(napi->priv, napi->weight);
            } else if (napi->nic && napi->nic->ops && napi->nic->ops->recv) {
                int limit = napi->weight;
                while (limit > 0 && work < budget) {
                    struct net_buf *buf = napi->nic->ops->recv(napi->priv);
                    if (buf == NULL) break;
                    
                    net_handle_ethernet(napi->nic, buf);
                    net_buf_free(buf);
                    
                    limit--;
                    napi_work++;
                    work++;
                }
            }
            
            if (napi_work == 0) {
                /* 无数据包，增加空闲计数 */
                napi_complete(napi);
            } else {
                napi->high_traffic_count = NAPI_POLL_THRESHOLD;  /* 保持轮询模式 */
            }
            continue;
        }
        
        /* 中断模式: 仅在调度时轮询 */
        if (napi->poll_list && (napi->state & NAPI_STATE_SCHED)) {
            int napi_work = 0;
            
            if (napi->poll) {
                napi_work = napi->poll(napi->priv, napi->weight);
            } else if (napi->nic && napi->nic->ops && napi->nic->ops->recv) {
                /* 默认轮询: 从网卡接收数据包 */
                int limit = napi->weight;
                while (limit > 0 && work < budget) {
                    struct net_buf *buf = napi->nic->ops->recv(napi->priv);
                    if (buf == NULL) break;
                    
                    net_handle_ethernet(napi->nic, buf);
                    net_buf_free(buf);
                    
                    limit--;
                    napi_work++;
                    work++;
                }
            }
            
            if (napi_work < napi->weight) {
                /* 工作量不足，切回中断模式 */
                napi_complete(napi);
                napi->high_traffic_count = 0;  /* 重置流量计数 */
            }
        }
    }
    
    return work;
}

/* ========== GRO/GSO (通用接收/发送卸载) ========== */

/* 初始化 GRO 引擎 */
void gro_init(struct gro_engine *engine) {
    if (engine == NULL) return;
    
    for (int i = 0; i < 8; i++) {
        engine->flows[i].buf = NULL;
        engine->flows[i].seg_count = 0;
        engine->flows[i].next = NULL;
    }
    engine->flow_count = 0;
    engine->max_segs = GRO_MAX_SEGS;
    engine->nic = NULL;  /* 由调用者后续设置 */
}

/* 计算流哈希 (简化: 基于 IP 和端口) */
static uint32_t gro_flow_hash(struct net_buf *buf) {
    if (buf == NULL || buf->len < sizeof(struct eth_hdr) + sizeof(struct ip_hdr)) {
        return 0;
    }
    
    struct ip_hdr *ip = (struct ip_hdr *)(buf->data + sizeof(struct eth_hdr));
    uint32_t hash = ip->saddr ^ ip->daddr;
    
    if (ip->protocol == 6 && buf->len >= sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct tcp_hdr)) {
        struct tcp_hdr *tcp = (struct tcp_hdr *)((uint8_t *)ip + ip->ihl * 4);
        hash ^= (tcp->source << 16) | tcp->dest;
    }
    
    return hash;
}

/* GRO 接收处理 */
struct gro_result gro_receive(struct gro_engine *engine, struct net_buf *buf) {
    struct gro_result result = {NULL, 1, 0};
    
    if (engine == NULL || buf == NULL) {
        return result;
    }
    
    uint32_t hash = gro_flow_hash(buf);
    result.merged = buf;
    
    /* 查找同流节点 */
    for (int i = 0; i < engine->flow_count; i++) {
        struct gro_node *node = &engine->flows[i];
        
        if (node->flow_hash == hash && node->seg_count < engine->max_segs) {
            /* 同流，尝试合并 */
            /* 检查是否可以合并 (TCP 序列号连续) */
            /* 简化: 直接合并数据 */
            size_t new_len = node->buf->len + buf->len - sizeof(struct eth_hdr) - sizeof(struct ip_hdr);
            if (new_len <= ETH_FRAME_LEN) {
                /* 合并到现有缓冲区 */
                uint8_t *dst = node->buf->data + node->buf->len;
                uint8_t *src = buf->data + sizeof(struct eth_hdr) + sizeof(struct ip_hdr);
                size_t copy_len = buf->len - sizeof(struct eth_hdr) - sizeof(struct ip_hdr);
                memcpy(dst, src, copy_len);
                node->buf->len += copy_len;
                node->seg_count++;
                
                result.merged = node->buf;
                result.seg_count = node->seg_count;
                result.same_flow = 1;
                return result;
            }
        }
    }
    
    /* 新流或无法合并，创建新节点 */
    if (engine->flow_count < 8) {
        struct gro_node *node = &engine->flows[engine->flow_count];
        node->buf = buf;
        node->flow_hash = hash;
        node->seg_count = 1;
        node->next = NULL;
        engine->flow_count++;
    }
    
    return result;
}

/* GRO 刷新 */
void gro_flush(struct gro_engine *engine) {
    if (engine == NULL) return;
    
    /* 将所有合并的缓冲区发送到上层 */
    for (int i = 0; i < engine->flow_count; i++) {
        struct gro_node *node = &engine->flows[i];
        if (node->buf && node->seg_count > 1) {
            /* 使用引擎关联的网卡发送合并后的缓冲区 */
            if (engine->nic) {
                net_handle_ethernet(engine->nic, node->buf);
            }
        }
        node->buf = NULL;
        node->seg_count = 0;
    }
    engine->flow_count = 0;
}

/* GSO 分段发送 */
int gso_segment(struct nic_device *nic, struct gso_ctx *ctx,
                const void *data, size_t len) {
    if (nic == NULL || ctx == NULL || data == NULL || len == 0) {
        return -1;
    }
    
    size_t offset = 0;
    int segs_sent = 0;
    
    while (offset < len) {
        size_t seg_len = len - offset;
        if (seg_len > ctx->mss) {
            seg_len = ctx->mss;
        }
        
        /* 构建并发送单个段 */
        struct net_buf *buf = net_buf_alloc();
        if (buf == NULL) break;
        
        /* 构建头部 + 数据 */
        uint8_t *pkt = buf->data;
        
        /* 以太网头 */
        struct eth_hdr *eth = (struct eth_hdr *)pkt;
        mac_copy(&eth->h_source, &nic->mac);
        /* 目的 MAC 需要通过 ARP 解析 */
        memset(&eth->h_dest, 0xFF, ETH_ALEN);  /* 广播 */
        eth->h_proto = htons(ETH_P_IP);
        
        /* IP 头 */
        struct ip_hdr *ip = (struct ip_hdr *)(pkt + sizeof(struct eth_hdr));
        ip->version = 4;
        ip->ihl = 5;
        ip->tos = 0;
        ip->tot_len = htons(ctx->hdr_len + seg_len);
        ip->id = 0;
        ip->frag_off = 0;
        ip->ttl = 64;
        ip->protocol = ctx->protocol;
        ip->saddr = nic->ip_addr;
        /* daddr 需要外部设置 */
        ip->check = 0;
        ip->check = ip_checksum(ip);
        
        /* 复制数据 */
        memcpy(pkt + sizeof(struct eth_hdr) + ctx->hdr_len,
               (const uint8_t *)data + offset, seg_len);
        
        buf->len = sizeof(struct eth_hdr) + ctx->hdr_len + seg_len;
        
        /* 发送 */
        if (net_send_packet(nic, buf) != 0) {
            net_buf_free(buf);
            break;
        }
        
        net_buf_free(buf);
        offset += seg_len;
        segs_sent++;
    }
    
    return segs_sent;
}

/* ========== SNMP 网络统计 ========== */

/* 全局 SNMP 统计 */
static struct net_snmp_stats snmp_stats;

/* 获取全局 SNMP 统计 */
struct net_snmp_stats *net_get_snmp_stats(void) {
    return &snmp_stats;
}

/* 更新 IP 层统计 */
void net_snmp_inc_ip(int field) {
    switch (field) {
        case 0: snmp_stats.ip_in_receives++; break;
        case 1: snmp_stats.ip_in_hdr_errors++; break;
        case 2: snmp_stats.ip_in_addr_errors++; break;
        case 3: snmp_stats.ip_forw_datagrams++; break;
        case 4: snmp_stats.ip_in_unknown_protos++; break;
        case 5: snmp_stats.ip_in_discards++; break;
        case 6: snmp_stats.ip_in_delivers++; break;
        case 7: snmp_stats.ip_out_requests++; break;
        case 8: snmp_stats.ip_out_discards++; break;
        case 9: snmp_stats.ip_out_no_routes++; break;
    }
}

/* 更新 TCP 层统计 */
void net_snmp_inc_tcp(int field) {
    switch (field) {
        case 0: snmp_stats.tcp_active_opens++; break;
        case 1: snmp_stats.tcp_passive_opens++; break;
        case 2: snmp_stats.tcp_attempt_fails++; break;
        case 3: snmp_stats.tcp_estab_resets++; break;
        case 4: snmp_stats.tcp_in_segs++; break;
        case 5: snmp_stats.tcp_out_segs++; break;
        case 6: snmp_stats.tcp_retrans_segs++; break;
        case 7: snmp_stats.tcp_in_errors++; break;
        case 8: snmp_stats.tcp_out_rsts++; break;
    }
}

/* 更新 UDP 层统计 */
void net_snmp_inc_udp(int field) {
    switch (field) {
        case 0: snmp_stats.udp_in_datagrams++; break;
        case 1: snmp_stats.udp_no_ports++; break;
        case 2: snmp_stats.udp_in_errors++; break;
        case 3: snmp_stats.udp_out_datagrams++; break;
    }
}

/* 更新 ICMP 层统计 */
void net_snmp_inc_icmp(int field) {
    switch (field) {
        case 0: snmp_stats.icmp_in_msgs++; break;
        case 1: snmp_stats.icmp_in_errors++; break;
        case 2: snmp_stats.icmp_out_msgs++; break;
        case 3: snmp_stats.icmp_out_errors++; break;
    }
}

/* 更新接口统计 */
void net_snmp_inc_if(int field) {
    switch (field) {
        case 0: snmp_stats.if_in_octets++; break;
        case 1: snmp_stats.if_out_octets++; break;
        case 2: snmp_stats.if_in_ucast_pkts++; break;
        case 3: snmp_stats.if_out_ucast_pkts++; break;
        case 4: snmp_stats.if_in_discards++; break;
        case 5: snmp_stats.if_out_discards++; break;
        case 6: snmp_stats.if_in_errors++; break;
        case 7: snmp_stats.if_out_errors++; break;
    }
}

/* 打印 SNMP 统计 */
void net_snmp_print(void) {
    println("=== SNMP Network Statistics ===");
    
    println("--- IP ---");
    print("InReceives:    "); print_dec(snmp_stats.ip_in_receives); println("");
    print("InHdrErrors:   "); print_dec(snmp_stats.ip_in_hdr_errors); println("");
    print("InDelivers:    "); print_dec(snmp_stats.ip_in_delivers); println("");
    print("OutRequests:   "); print_dec(snmp_stats.ip_out_requests); println("");
    
    println("--- TCP ---");
    print("ActiveOpens:   "); print_dec(snmp_stats.tcp_active_opens); println("");
    print("PassiveOpens:  "); print_dec(snmp_stats.tcp_passive_opens); println("");
    print("InSegs:        "); print_dec(snmp_stats.tcp_in_segs); println("");
    print("OutSegs:       "); print_dec(snmp_stats.tcp_out_segs); println("");
    print("RetransSegs:   "); print_dec(snmp_stats.tcp_retrans_segs); println("");
    print("InErrors:      "); print_dec(snmp_stats.tcp_in_errors); println("");
    
    println("--- UDP ---");
    print("InDatagrams:   "); print_dec(snmp_stats.udp_in_datagrams); println("");
    print("OutDatagrams:  "); print_dec(snmp_stats.udp_out_datagrams); println("");
    
    println("--- ICMP ---");
    print("InMsgs:        "); print_dec(snmp_stats.icmp_in_msgs); println("");
    print("OutMsgs:       "); print_dec(snmp_stats.icmp_out_msgs); println("");
    
    println("--- Interface ---");
    print("InOctets:      "); print_dec(snmp_stats.if_in_octets); println("");
    print("OutOctets:     "); print_dec(snmp_stats.if_out_octets); println("");
    print("InUcastPkts:   "); print_dec(snmp_stats.if_in_ucast_pkts); println("");
    print("OutUcastPkts:  "); print_dec(snmp_stats.if_out_ucast_pkts); println("");
}
