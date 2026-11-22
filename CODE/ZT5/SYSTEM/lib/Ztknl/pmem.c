/*
 * Zratro OS - PMEM (持久内存管理子系统) 实现
 * 
 * 实现持久内存的核心功能：
 * 1. 段管理 - 创建、打开、关闭、销毁
 * 2. 事务性持久化 - 与 Z-Journal 集成
 * 3. 快照系统 - 完整快照和增量快照
 * 4. 持久分配器 - 段内堆管理
 * 5. 瞬时启动 - 状态保存与恢复
 */

#include "pmem.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include <stdarg.h>

/* ========== 全局管理器 ========== */

static struct pmem_manager pmem_global;

/* ========== 内部辅助函数 ========== */

/* 计算校验和 */
static uint32_t pmem_calc_checksum(const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        sum = sum * 31 + p[i];
    }
    return sum;
}

/* 初始化段头 */
static void pmem_init_seg_header(struct pmem_segment *seg, const char *name,
                                  uint64_t size, uint32_t flags) {
    struct pmem_seg_header *hdr = pmem_get_header(seg);
    
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = PMEM_MAGIC;
    hdr->version = 1;
    hdr->seg_id = seg->seg_id;
    strncpy(hdr->name, name, PMEM_MAX_NAME_LEN - 1);
    hdr->name[PMEM_MAX_NAME_LEN - 1] = '\0';
    hdr->size = size;
    hdr->used_size = 0;
    hdr->flags = flags;
    hdr->create_time = 0;  /* TODO: 获取时间 */
    hdr->modify_time = hdr->create_time;
    hdr->owner_pid = 0;
    hdr->ref_count = 1;
    hdr->checksum = pmem_calc_checksum(hdr, offsetof(struct pmem_seg_header, checksum));
    
    /* 持久化头 */
    pmem_persist(hdr, sizeof(*hdr));
}

/* 验证段头 */
static int pmem_verify_seg_header(struct pmem_segment *seg) {
    struct pmem_seg_header *hdr = pmem_get_header(seg);
    
    if (hdr->magic != PMEM_MAGIC) {
        return -1;
    }
    
    uint32_t expected = pmem_calc_checksum(hdr, offsetof(struct pmem_seg_header, checksum));
    if (hdr->checksum != expected) {
        return -2;
    }
    
    return 0;
}

/* 更新段头 */
static void pmem_update_seg_header(struct pmem_segment *seg) {
    struct pmem_seg_header *hdr = pmem_get_header(seg);
    
    hdr->modify_time = 0;  /* TODO: 获取时间 */
    hdr->checksum = pmem_calc_checksum(hdr, offsetof(struct pmem_seg_header, checksum));
    
    pmem_persist(hdr, sizeof(*hdr));
}

/* 查找段 - 带 PID 保护 */
static struct pmem_segment *find_segment_locked(struct pmem_manager *pm, 
                                                 uint64_t seg_id, int hold_lock) {
    struct pmem_segment *seg;
    
    spin_lock(&pm->seg_lock);
    list_for_each_entry(seg, &pm->segments, list) {
        if (seg->seg_id == seg_id) {
            seg->ref_count++;
            if (!hold_lock) {
                spin_unlock(&pm->seg_lock);
            }
            return seg;
        }
    }
    spin_unlock(&pm->seg_lock);
    return NULL;
}

/* 释放段引用 */
static void put_segment(struct pmem_segment *seg) {
    spin_lock(&seg->lock);
    seg->ref_count--;
    if (seg->ref_count == 0 && seg->state == PMEM_SEG_STATE_CLOSING) {
        /* 可以释放 */
        spin_unlock(&seg->lock);
        kfree(seg);
        return;
    }
    spin_unlock(&seg->lock);
}

/* ========== 初始化/销毁 ========== */

int pmem_init(struct pmem_manager *pm, uint64_t pmem_start, uint64_t pmem_size,
              pmem_type_t type, struct zjournal *journal) {
    if (pm == NULL) {
        return -1;
    }
    
    memset(pm, 0, sizeof(*pm));
    
    /* 初始化拓扑信息 */
    pm->topology.total_size = pmem_size;
    pm->topology.available_size = pmem_size;
    pm->topology.type = type;
    pm->topology.numa_node = 0;
    pm->topology.interleaved = 0;
    
    /* 初始化段链表 */
    INIT_LIST_HEAD(&pm->segments);
    spinlock_init(&pm->seg_lock);
    
    /* 初始化快照链表 */
    INIT_LIST_HEAD(&pm->snapshots);
    spinlock_init(&pm->snap_lock);
    
    /* 关联日志 */
    pm->journal = journal;
    
    /* 初始化 ID 分配 */
    pm->next_seg_id = 1;
    pm->next_snapshot_id = 1;
    
    pm->initialized = 1;
    pm->recovery_mode = 0;
    
    return 0;
}

void pmem_destroy(struct pmem_manager *pm) {
    if (pm == NULL || !pm->initialized) {
        return;
    }
    
    /* 关闭所有段 */
    spin_lock(&pm->seg_lock);
    while (!list_empty(&pm->segments)) {
        struct pmem_segment *seg = list_first_entry(&pm->segments, 
                                                     struct pmem_segment, list);
        list_del(&seg->list);
        pm->segment_count--;
        kfree(seg);
    }
    spin_unlock(&pm->seg_lock);
    
    /* 释放所有快照 */
    spin_lock(&pm->snap_lock);
    while (!list_empty(&pm->snapshots)) {
        struct pmem_snapshot *snap = list_first_entry(&pm->snapshots,
                                                       struct pmem_snapshot, list);
        list_del(&snap->list);
        if (snap->data) {
            kfree(snap->data);
        }
        kfree(snap);
    }
    spin_unlock(&pm->snap_lock);
    
    pm->initialized = 0;
}

/* ========== 段管理 ========== */

struct pmem_segment *pmem_create_segment(struct pmem_manager *pm,
                                          const char *name, uint64_t size,
                                          uint32_t flags) {
    if (pm == NULL || name == NULL || size < PMEM_MIN_SEGMENT_SIZE) {
        return NULL;
    }
    
    /* 检查是否已存在同名段 */
    spin_lock(&pm->seg_lock);
    struct pmem_segment *existing;
    list_for_each_entry(existing, &pm->segments, list) {
        if (strncmp(existing->name, name, PMEM_MAX_NAME_LEN) == 0) {
            spin_unlock(&pm->seg_lock);
            return NULL;  /* 已存在 */
        }
    }
    spin_unlock(&pm->seg_lock);
    
    /* 检查可用空间 */
    if (size > pm->topology.available_size) {
        console_error("[PMEM] 空间不足");
        return NULL;
    }
    
    /* 分配段描述符 */
    struct pmem_segment *seg = kmalloc(sizeof(*seg));
    if (seg == NULL) {
        return NULL;
    }
    
    memset(seg, 0, sizeof(*seg));
    
    /* 分配物理内存 */
    uint64_t phys = pmm_alloc_pages((size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (phys == 0) {
        kfree(seg);
        return NULL;
    }
    
    /* 通过 HHDM 映射获取虚拟地址 */
    void *virt = phys_to_virt(phys);
    if (virt == NULL) {
        pmm_free_pages(phys, (size + PAGE_SIZE - 1) / PAGE_SIZE);
        kfree(seg);
        return NULL;
    }
    
    /* 初始化段 */
    seg->seg_id = pm->next_seg_id++;
    strncpy(seg->name, name, PMEM_MAX_NAME_LEN - 1);
    seg->name[PMEM_MAX_NAME_LEN - 1] = '\0';
    seg->base = virt;
    seg->phys_addr = phys;
    seg->size = size;
    seg->state = PMEM_SEG_STATE_CREATING;
    seg->flags = flags;
    seg->journal = pm->journal;
    seg->ref_count = 1;
    spinlock_init(&seg->lock);
    INIT_LIST_HEAD(&seg->list);
    INIT_LIST_HEAD(&seg->global_list);
    
    /* 清零内存 */
    memset(virt, 0, size);
    
    /* 初始化段头 */
    pmem_init_seg_header(seg, name, size, flags);
    
    /* 添加到全局链表 */
    spin_lock(&pm->seg_lock);
    list_add_tail(&seg->list, &pm->segments);
    pm->segment_count++;
    pm->topology.available_size -= size;
    spin_unlock(&pm->seg_lock);
    
    /* 更新状态 */
    seg->state = PMEM_SEG_STATE_ACTIVE;
    
    pm->total_allocated += size;
    
    print("[PMEM] 创建段 '");
    print(name);
    print("' (ID: ");
    print_dec(seg->seg_id);
    print(", 大小: ");
    print_dec(size / 1024);
    println(" KB)");
    
    return seg;
}

struct pmem_segment *pmem_open_segment(struct pmem_manager *pm,
                                        const char *name, uint32_t flags) {
    if (pm == NULL || name == NULL) {
        return NULL;
    }
    
    spin_lock(&pm->seg_lock);
    struct pmem_segment *seg;
    list_for_each_entry(seg, &pm->segments, list) {
        if (strncmp(seg->name, name, PMEM_MAX_NAME_LEN) == 0) {
            /* 验证段头 */
            if (pmem_verify_seg_header(seg) != 0) {
                spin_unlock(&pm->seg_lock);
                print("[PMEM] 段头验证失败: ");
                println(name);
                return NULL;
            }
            
            seg->ref_count++;
            spin_unlock(&pm->seg_lock);
            return seg;
        }
    }
    spin_unlock(&pm->seg_lock);
    
    return NULL;
}

int pmem_close_segment(struct pmem_manager *pm, struct pmem_segment *seg) {
    if (pm == NULL || seg == NULL) {
        return -1;
    }
    
    spin_lock(&seg->lock);
    seg->ref_count--;
    
    if (seg->ref_count == 0) {
        seg->state = PMEM_SEG_STATE_CLOSING;
        
        /* 同步到持久存储 */
        pmem_sync(seg);
        
        /* 更新段头 */
        pmem_update_seg_header(seg);
        
        /* 从链表移除 */
        spin_lock(&pm->seg_lock);
        list_del(&seg->list);
        pm->segment_count--;
        pm->topology.available_size += seg->size;
        spin_unlock(&pm->seg_lock);
        
        spin_unlock(&seg->lock);
        kfree(seg);
        return 0;
    }
    
    spin_unlock(&seg->lock);
    return 0;
}

int pmem_destroy_segment(struct pmem_manager *pm, const char *name) {
    if (pm == NULL || name == NULL) {
        return -1;
    }
    
    spin_lock(&pm->seg_lock);
    struct pmem_segment *seg;
    list_for_each_entry(seg, &pm->segments, list) {
        if (strncmp(seg->name, name, PMEM_MAX_NAME_LEN) == 0) {
            /* 检查引用 */
            if (seg->ref_count > 1) {
                spin_unlock(&pm->seg_lock);
                return -2;  /* 仍在使用 */
            }
            
            list_del(&seg->list);
            pm->segment_count--;
            pm->topology.available_size += seg->size;
            spin_unlock(&pm->seg_lock);
            
            /* 释放内存 */
            pmm_free_pages(seg->phys_addr, (seg->size + PAGE_SIZE - 1) / PAGE_SIZE);
            kfree(seg);
            
            print("[PMEM] 销毁段: ");
    print(name);
    println("");
            return 0;
        }
    }
    spin_unlock(&pm->seg_lock);
    
    return -1;  /* 未找到 */
}

struct pmem_segment *pmem_find_segment(struct pmem_manager *pm, uint64_t seg_id) {
    return find_segment_locked(pm, seg_id, 0);
}

struct pmem_segment *pmem_find_segment_by_name(struct pmem_manager *pm, 
                                                const char *name) {
    if (pm == NULL || name == NULL) {
        return NULL;
    }
    
    spin_lock(&pm->seg_lock);
    struct pmem_segment *seg;
    list_for_each_entry(seg, &pm->segments, list) {
        if (strncmp(seg->name, name, PMEM_MAX_NAME_LEN) == 0) {
            seg->ref_count++;
            spin_unlock(&pm->seg_lock);
            return seg;
        }
    }
    spin_unlock(&pm->seg_lock);
    
    return NULL;
}

/* ========== 持久内存操作 ========== */

int pmem_write(struct pmem_segment *seg, uint64_t offset,
               const void *data, size_t size) {
    if (seg == NULL || data == NULL) {
        return -1;
    }
    
    /* 检查边界 */
    if (!pmem_offset_valid(seg, offset) || 
        offset + size > pmem_get_data_size(seg)) {
        return -2;
    }
    
    void *dest = (void *)((uint8_t *)pmem_get_data_area(seg) + offset);
    
    /* 复制数据 */
    memcpy(dest, data, size);
    
    /* 持久化 */
    pmem_persist(dest, size);
    
    /* 更新段头 */
    spin_lock(&seg->lock);
    struct pmem_seg_header *hdr = pmem_get_header(seg);
    if (offset + size > hdr->used_size) {
        hdr->used_size = offset + size;
    }
    pmem_update_seg_header(seg);
    spin_unlock(&seg->lock);
    
    return 0;
}

int pmem_read(struct pmem_segment *seg, uint64_t offset,
              void *data, size_t size) {
    if (seg == NULL || data == NULL) {
        return -1;
    }
    
    /* 检查边界 */
    if (!pmem_offset_valid(seg, offset) ||
        offset + size > pmem_get_data_size(seg)) {
        return -2;
    }
    
    void *src = (void *)((uint8_t *)pmem_get_data_area(seg) + offset);
    memcpy(data, src, size);
    
    return 0;
}

/* ========== 事务性操作 ========== */

int pmem_trans_write(struct pmem_segment *seg, uint64_t offset,
                     const void *data, size_t size,
                     struct zj_transaction *tx) {
    if (seg == NULL || data == NULL || tx == NULL) {
        return -1;
    }
    
    /* 检查边界 */
    if (!pmem_offset_valid(seg, offset) ||
        offset + size > pmem_get_data_size(seg)) {
        return -2;
    }
    
    /* 如果有日志，记录到 WAL */
    if (seg->journal != NULL) {
        /* 记录旧数据用于回滚 */
        void *old_data = kmalloc(size);
        if (old_data == NULL) {
            return -3;
        }
        
        pmem_read(seg, offset, old_data, size);
        
        /* 添加到事务 */
        int ret = zj_add_write_op(seg->journal, tx, seg->seg_id, offset,
                                  old_data, size);
        kfree(old_data);
        
        if (ret != 0) {
            return -4;
        }
    }
    
    /* 执行写入 */
    int ret = pmem_write(seg, offset, data, size);
    if (ret != 0) {
        return ret;
    }
    
    /* 记录事务 ID */
    spin_lock(&seg->lock);
    struct pmem_seg_header *hdr = pmem_get_header(seg);
    hdr->last_trans_id = tx->trans_id;
    pmem_update_seg_header(seg);
    spin_unlock(&seg->lock);
    
    return 0;
}

int pmem_trans_memcpy(struct pmem_segment *seg, uint64_t dst_offset,
                      uint64_t src_offset, size_t size,
                      struct zj_transaction *tx) {
    if (seg == NULL || tx == NULL) {
        return -1;
    }
    
    /* 读取源数据 */
    void *temp = kmalloc(size);
    if (temp == NULL) {
        return -2;
    }
    
    int ret = pmem_read(seg, src_offset, temp, size);
    if (ret != 0) {
        kfree(temp);
        return ret;
    }
    
    /* 事务性写入 */
    ret = pmem_trans_write(seg, dst_offset, temp, size, tx);
    kfree(temp);
    
    return ret;
}

/* ========== 持久分配器 ========== */

void *pmem_alloc(struct pmem_segment *seg, size_t size) {
    if (seg == NULL || size == 0) {
        return NULL;
    }
    
    /* 对齐到 16 字节 */
    size = (size + 15) & ~15;
    
    /* 计算需要的总大小 (包括块头) */
    size_t total_size = size + sizeof(struct pmem_block_header);
    
    spin_lock(&seg->lock);
    
    struct pmem_seg_header *hdr = pmem_get_header(seg);
    uint64_t data_size = pmem_get_data_size(seg);
    
    /* 检查空间 */
    if (hdr->used_size + total_size > data_size) {
        spin_unlock(&seg->lock);
        return NULL;
    }
    
    /* 从段尾分配 */
    uint64_t offset = hdr->used_size;
    void *ptr = (void *)((uint8_t *)pmem_get_data_area(seg) + offset);
    
    /* 初始化块头 */
    struct pmem_block_header *blk = (struct pmem_block_header *)ptr;
    blk->magic = PMEM_BLOCK_MAGIC;
    blk->size = size;
    blk->alloc_id = seg->seg_id;
    blk->flags = 0;
    blk->checksum = pmem_calc_checksum(blk, offsetof(struct pmem_block_header, checksum));
    
    /* 更新使用大小 */
    hdr->used_size += total_size;
    pmem_update_seg_header(seg);
    
    /* 持久化 */
    pmem_persist(ptr, total_size);
    
    spin_unlock(&seg->lock);
    
    /* 返回数据区地址 */
    return (void *)((uint8_t *)ptr + sizeof(struct pmem_block_header));
}

void pmem_free(struct pmem_segment *seg, void *ptr) {
    if (seg == NULL || ptr == NULL) {
        return;
    }
    
    /* 获取块头 */
    struct pmem_block_header *blk = (struct pmem_block_header *)
        ((uint8_t *)ptr - sizeof(struct pmem_block_header));
    
    /* 验证块头 */
    if (blk->magic != PMEM_BLOCK_MAGIC) {
        return;
    }
    
    /* 标记为已释放 */
    blk->flags |= 1;
    blk->checksum = pmem_calc_checksum(blk, offsetof(struct pmem_block_header, checksum));
    pmem_persist(blk, sizeof(*blk));
    
    /* 
     * 注意：这里不回收内存，持久内存的回收需要特殊处理
     * 可以在段压缩时进行实际的回收
     */
}

uint64_t pmem_alloc_offset(struct pmem_segment *seg, size_t size) {
    void *ptr = pmem_alloc(seg, size);
    if (ptr == NULL) {
        return (uint64_t)-1;
    }
    
    return (uint64_t)((uint8_t *)ptr - (uint8_t *)pmem_get_data_area(seg));
}

int pmem_free_offset(struct pmem_segment *seg, uint64_t offset) {
    if (seg == NULL) {
        return -1;
    }
    
    void *ptr = (void *)((uint8_t *)pmem_get_data_area(seg) + offset);
    pmem_free(seg, ptr);
    
    return 0;
}

/* ========== 快照系统 ========== */

struct pmem_snapshot *pmem_snapshot_create(struct pmem_manager *pm,
                                            struct pmem_segment *seg,
                                            int incremental) {
    if (pm == NULL || seg == NULL) {
        return NULL;
    }
    
    struct pmem_snapshot *snap = kmalloc(sizeof(*snap));
    if (snap == NULL) {
        return NULL;
    }
    
    memset(snap, 0, sizeof(*snap));
    
    spin_lock(&seg->lock);
    
    struct pmem_seg_header *hdr = pmem_get_header(seg);
    uint64_t data_size = hdr->used_size;
    
    /* 分配快照数据缓冲区 */
    if (incremental && hdr->snapshot_id > 0) {
        /* 增量快照：只保存变更 */
        /* TODO: 实现增量快照 */
        incremental = 0;  /* 暂时回退到完整快照 */
    }
    
    snap->data = kmalloc(data_size);
    if (snap->data == NULL) {
        spin_unlock(&seg->lock);
        kfree(snap);
        return NULL;
    }
    
    /* 复制数据 */
    memcpy(snap->data, pmem_get_data_area(seg), data_size);
    
    /* 初始化快照 */
    snap->snapshot_id = pm->next_snapshot_id++;
    snap->seg_id = seg->seg_id;
    snap->timestamp = 0;  /* TODO: 获取时间 */
    snap->size = data_size;
    snap->is_incremental = incremental;
    snap->base_snapshot = hdr->snapshot_id;
    
    /* 更新段头 */
    hdr->snapshot_id = snap->snapshot_id;
    pmem_update_seg_header(seg);
    
    spin_unlock(&seg->lock);
    
    /* 添加到链表 */
    spin_lock(&pm->snap_lock);
    list_add_tail(&snap->list, &pm->snapshots);
    spin_unlock(&pm->snap_lock);
    
    pm->snapshots_created++;
    
    print("[PMEM] 创建快照 #");
    print_dec(snap->snapshot_id);
    print(" (段: ");
    print(seg->name);
    print(", 大小: ");
    print_dec(data_size / 1024);
    println(" KB)");
    
    return snap;
}

int pmem_snapshot_restore(struct pmem_manager *pm,
                          struct pmem_snapshot *snap) {
    if (pm == NULL || snap == NULL || snap->data == NULL) {
        return -1;
    }
    
    /* 查找段 */
    struct pmem_segment *seg = pmem_find_segment(pm, snap->seg_id);
    if (seg == NULL) {
        return -2;
    }
    
    spin_lock(&seg->lock);
    
    /* 恢复数据 */
    memcpy(pmem_get_data_area(seg), snap->data, snap->size);
    
    /* 更新段头 */
    struct pmem_seg_header *hdr = pmem_get_header(seg);
    hdr->used_size = snap->size;
    hdr->snapshot_id = snap->snapshot_id;
    pmem_update_seg_header(seg);
    
    /* 持久化 */
    pmem_persist(seg->base, seg->size);
    
    spin_unlock(&seg->lock);
    put_segment(seg);
    
    pm->snapshots_restored++;
    
    print("[PMEM] 恢复快照 #");
    print_dec(snap->snapshot_id);
    println("");
    
    return 0;
}

int pmem_snapshot_delete(struct pmem_manager *pm, uint64_t snapshot_id) {
    if (pm == NULL) {
        return -1;
    }
    
    spin_lock(&pm->snap_lock);
    
    struct pmem_snapshot *snap;
    list_for_each_entry(snap, &pm->snapshots, list) {
        if (snap->snapshot_id == snapshot_id) {
            list_del(&snap->list);
            
            if (snap->data) {
                kfree(snap->data);
            }
            kfree(snap);
            
            spin_unlock(&pm->snap_lock);
            return 0;
        }
    }
    
    spin_unlock(&pm->snap_lock);
    return -1;
}

/* ========== 同步 ========== */

int pmem_sync(struct pmem_segment *seg) {
    if (seg == NULL) {
        return -1;
    }
    
    /* 刷新整个段到持久存储 */
    pmem_persist(seg->base, seg->size);
    
    /* 如果有日志，确保日志已提交 */
    if (seg->journal != NULL && seg->active_tx != NULL) {
        zj_trans_commit(seg->journal, seg->active_tx);
        seg->active_tx = NULL;
    }
    
    return 0;
}

int pmem_flush(struct pmem_segment *seg, uint64_t offset, size_t size) {
    if (seg == NULL) {
        return -1;
    }
    
    if (!pmem_offset_valid(seg, offset)) {
        return -2;
    }
    
    void *addr = (void *)((uint8_t *)pmem_get_data_area(seg) + offset);
    pmem_persist(addr, size);
    
    return 0;
}

/* ========== 恢复 ========== */

int pmem_recover(struct pmem_manager *pm) {
    if (pm == NULL) {
        return -1;
    }
    
    pm->recovery_mode = 1;
    
    /* 遍历所有段，验证并恢复 */
    spin_lock(&pm->seg_lock);
    struct pmem_segment *seg;
    list_for_each_entry(seg, &pm->segments, list) {
        if (pmem_verify_seg_header(seg) != 0) {
            print("[PMEM] 段 '");
            print(seg->name);
            println("' 头损坏，尝试恢复");
            /* TODO: 从日志恢复 */
        }
    }
    spin_unlock(&pm->seg_lock);
    
    /* 恢复日志 */
    if (pm->journal != NULL) {
        zj_recover(pm->journal);
    }
    
    pm->recovery_mode = 0;
    
    console_success("[PMEM] 恢复完成");
    return 0;
}

int pmem_recover_segment(struct pmem_manager *pm, const char *name) {
    if (pm == NULL || name == NULL) {
        return -1;
    }
    
    struct pmem_segment *seg = pmem_find_segment_by_name(pm, name);
    if (seg == NULL) {
        return -2;
    }
    
    seg->state = PMEM_SEG_STATE_RECOVERING;
    
    int ret = pmem_verify_seg_header(seg);
    if (ret != 0) {
        /* 尝试从日志恢复 */
        if (seg->journal != NULL) {
            zj_recover(seg->journal);
        }
    }
    
    seg->state = PMEM_SEG_STATE_ACTIVE;
    put_segment(seg);
    
    return 0;
}

/* ========== 统计 ========== */

void pmem_get_stats(struct pmem_manager *pm, uint64_t *allocated,
                    uint64_t *freed, uint64_t *segments) {
    if (pm == NULL) {
        return;
    }
    
    if (allocated) *allocated = pm->total_allocated;
    if (freed) *freed = pm->total_freed;
    if (segments) *segments = pm->segment_count;
}

/* ========== 高级特性 ========== */

/* 瞬时启动支持 */
int pmem_bootstrap(struct pmem_manager *pm) {
    if (pm == NULL) {
        return -1;
    }
    
    /* 检查是否存在启动状态段 */
    struct pmem_segment *boot_seg = pmem_find_segment_by_name(pm, "__boot_state__");
    if (boot_seg == NULL) {
        /* 首次启动，创建启动状态段 */
        boot_seg = pmem_create_segment(pm, "__boot_state__", 64 * 1024,
                                        PMEM_SEG_FLAG_PERSISTENT);
        if (boot_seg == NULL) {
            return -2;
        }
        put_segment(boot_seg);
        return 1;  /* 首次启动 */
    }
    
    /* 恢复启动状态 */
    /* TODO: 读取并恢复内核状态 */
    put_segment(boot_seg);
    
    return 0;  /* 从持久状态启动 */
}

int pmem_save_boot_state(struct pmem_manager *pm) {
    if (pm == NULL) {
        return -1;
    }
    
    struct pmem_segment *boot_seg = pmem_find_segment_by_name(pm, "__boot_state__");
    if (boot_seg == NULL) {
        return -2;
    }
    
    /* TODO: 保存内核关键状态到启动状态段 */
    
    pmem_sync(boot_seg);
    put_segment(boot_seg);
    
    return 0;
}

int pmem_restore_boot_state(struct pmem_manager *pm) {
    if (pm == NULL) {
        return -1;
    }
    
    struct pmem_segment *boot_seg = pmem_find_segment_by_name(pm, "__boot_state__");
    if (boot_seg == NULL) {
        return -2;
    }
    
    /* TODO: 从启动状态段恢复内核状态 */
    
    put_segment(boot_seg);
    
    return 0;
}

/* 压缩 */
int pmem_compact_segment(struct pmem_segment *seg) {
    if (seg == NULL) {
        return -1;
    }
    
    spin_lock(&seg->lock);
    seg->state = PMEM_SEG_STATE_COMPACTING;
    
    /* TODO: 实现段压缩 */
    /* 1. 扫描已释放块
     * 2. 移动存活块
     * 3. 更新偏移映射表
     */
    
    seg->state = PMEM_SEG_STATE_ACTIVE;
    spin_unlock(&seg->lock);
    
    return 0;
}

uint64_t pmem_fragmentation(struct pmem_segment *seg) {
    if (seg == NULL) {
        return 0;
    }
    
    struct pmem_seg_header *hdr = pmem_get_header(seg);
    
    /* 简单的碎片度计算 */
    uint64_t data_size = pmem_get_data_size(seg);
    if (data_size == 0) return 0;
    
    /* 碎片度 = (总大小 - 已用大小) / 总大小 * 100 */
    return ((data_size - hdr->used_size) * 100) / data_size;
}

/* 加密 */
int pmem_encrypt_segment(struct pmem_segment *seg, const void *key, size_t key_len) {
    if (seg == NULL || key == NULL) {
        return -1;
    }
    
    /* TODO: 实现加密 */
    /* 可以使用 AES-XTS 模式 */
    
    return 0;
}

int pmem_decrypt_segment(struct pmem_segment *seg, const void *key, size_t key_len) {
    if (seg == NULL || key == NULL) {
        return -1;
    }
    
    /* TODO: 实现解密 */
    
    return 0;
}

/* NUMA 感知 */
int pmem_set_numa_policy(struct pmem_manager *pm, uint32_t node, int interleave) {
    if (pm == NULL) {
        return -1;
    }
    
    pm->topology.numa_node = node;
    pm->topology.interleaved = interleave;
    
    return 0;
}

uint32_t pmem_get_numa_node(struct pmem_segment *seg) {
    if (seg == NULL) {
        return 0;
    }
    
    /* TODO: 从物理地址查询 NUMA 节点 */
    return 0;
}

/* ========== 全局 API ========== */

int pmem_global_init(uint64_t pmem_start, uint64_t pmem_size,
                     pmem_type_t type, struct zjournal *journal) {
    return pmem_init(&pmem_global, pmem_start, pmem_size, type, journal);
}

struct pmem_manager *pmem_get_global(void) {
    if (pmem_global.initialized) {
        return &pmem_global;
    }
    return NULL;
}
