/*
 * Zratro OS - Heterogeneous Computing Scheduler (异构计算统一调度)
 * 
 * 实现：
 * 1. 设备注册与管理
 * 2. 内存分配与迁移
 * 3. 任务调度与执行
 * 4. 自动设备选择
 */

#include "heterog.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "task.h"

/* ========== 全局调度器 ========== */

static struct het_scheduler global_hs;

/* ========== 初始化/销毁 ========== */

int het_init(struct het_scheduler *hs) {
    if (hs == NULL) {
        return -1;
    }
    
    memset(hs, 0, sizeof(*hs));
    
    INIT_LIST_HEAD(&hs->devices);
    INIT_LIST_HEAD(&hs->queues);
    INIT_LIST_HEAD(&hs->all_tasks);
    INIT_LIST_HEAD(&hs->events);
    INIT_LIST_HEAD(&hs->memory_regions);
    
    spinlock_init(&hs->lock);
    
    /* 默认调度策略 */
    hs->policy = HET_POLICY_PERF_MODEL;
    hs->flags = HET_FLAG_AUTO_MIGRATE;
    
    /* 初始化性能模型权重 (定点数，精度 1000) */
    hs->perf_model.compute_weight = 500;   /* 0.5 * 1000 */
    hs->perf_model.memory_weight = 300;    /* 0.3 * 1000 */
    hs->perf_model.latency_weight = 200;   /* 0.2 * 1000 */
    
    return 0;
}

void het_destroy(struct het_scheduler *hs) {
    if (hs == NULL) {
        return;
    }
    
    spin_lock(&hs->lock);
    
    /* 释放所有内存区域 */
    struct het_memory_region *mem, *mem_tmp;
    list_for_each_entry_safe(mem, mem_tmp, &hs->memory_regions, list) {
        list_del(&mem->list);
        if (mem->host_ptr != NULL && mem->type != HET_MEM_DEVICE) {
            kfree(mem->host_ptr);
        }
        kfree(mem);
    }
    
    /* 释放所有任务 */
    struct het_task *task, *task_tmp;
    list_for_each_entry_safe(task, task_tmp, &hs->all_tasks, list) {
        list_del(&task->list);
        if (task->args != NULL) {
            kfree(task->args);
        }
        kfree(task);
    }
    
    /* 释放所有事件 */
    struct het_event *evt, *evt_tmp;
    list_for_each_entry_safe(evt, evt_tmp, &hs->events, list) {
        list_del(&evt->list);
        kfree(evt);
    }
    
    /* 释放所有队列 */
    struct het_queue *q, *q_tmp;
    list_for_each_entry_safe(q, q_tmp, &hs->queues, list) {
        list_del(&q->list);
        kfree(q);
    }
    
    /* 释放所有设备 */
    struct het_device *dev, *dev_tmp;
    list_for_each_entry_safe(dev, dev_tmp, &hs->devices, list) {
        list_del(&dev->list);
        kfree(dev);
    }
    
    spin_unlock(&hs->lock);
}

/* ========== 设备管理 ========== */

int het_register_device(struct het_scheduler *hs, struct het_device *dev) {
    if (hs == NULL || dev == NULL) {
        return -1;
    }
    
    spin_lock(&hs->lock);
    
    dev->device_id = hs->device_count++;
    INIT_LIST_HEAD(&dev->queues);
    dev->is_available = 1;
    
    list_add_tail(&dev->list, &hs->devices);
    
    spin_unlock(&hs->lock);
    
    return 0;
}

int het_unregister_device(struct het_scheduler *hs, uint32_t device_id) {
    if (hs == NULL) {
        return -1;
    }
    
    spin_lock(&hs->lock);
    
    struct het_device *dev;
    list_for_each_entry(dev, &hs->devices, list) {
        if (dev->device_id == device_id) {
            dev->is_available = 0;
            list_del(&dev->list);
            hs->device_count--;
            spin_unlock(&hs->lock);
            return 0;
        }
    }
    
    spin_unlock(&hs->lock);
    return -1;
}

struct het_device *het_get_device(struct het_scheduler *hs, uint32_t device_id) {
    if (hs == NULL) {
        return NULL;
    }
    
    spin_lock(&hs->lock);
    
    struct het_device *dev;
    list_for_each_entry(dev, &hs->devices, list) {
        if (dev->device_id == device_id && dev->is_available) {
            spin_unlock(&hs->lock);
            return dev;
        }
    }
    
    spin_unlock(&hs->lock);
    return NULL;
}

int het_query_capability(struct het_device *dev, uint64_t cap) {
    if (dev == NULL) {
        return 0;
    }
    return (dev->capabilities & cap) != 0;
}

int het_enumerate_devices(struct het_scheduler *hs, het_dev_type_t type,
                          uint32_t *devices, uint32_t *count) {
    if (hs == NULL || devices == NULL || count == NULL) {
        return -1;
    }
    
    spin_lock(&hs->lock);
    
    uint32_t found = 0;
    struct het_device *dev;
    list_for_each_entry(dev, &hs->devices, list) {
        if (dev->is_available && (type == HET_DEV_NONE || dev->type == type)) {
            if (found < *count) {
                devices[found] = dev->device_id;
            }
            found++;
        }
    }
    
    *count = found;
    spin_unlock(&hs->lock);
    
    return 0;
}

/* ========== 队列管理 ========== */

struct het_queue *het_create_queue(struct het_scheduler *hs,
                                    uint32_t device_id,
                                    uint32_t priority,
                                    uint32_t flags) {
    if (hs == NULL) {
        return NULL;
    }
    
    struct het_device *dev = het_get_device(hs, device_id);
    if (dev == NULL) {
        return NULL;
    }
    
    struct het_queue *queue = kmalloc(sizeof(*queue));
    if (queue == NULL) {
        return NULL;
    }
    
    memset(queue, 0, sizeof(*queue));
    
    queue->device_id = device_id;
    queue->priority = priority;
    queue->flags = flags;
    
    INIT_LIST_HEAD(&queue->pending);
    INIT_LIST_HEAD(&queue->running);
    INIT_LIST_HEAD(&queue->completed);
    INIT_LIST_HEAD(&queue->list);
    INIT_LIST_HEAD(&queue->global_list);
    
    spin_lock(&hs->lock);
    queue->queue_id = hs->queue_count++;
    list_add_tail(&queue->global_list, &hs->queues);  /* 全局链表 */
    list_add_tail(&queue->list, &dev->queues);        /* 设备链表 */
    dev->queue_count++;
    spin_unlock(&hs->lock);
    
    return queue;
}

int het_destroy_queue(struct het_scheduler *hs, uint32_t queue_id) {
    if (hs == NULL) {
        return -1;
    }
    
    spin_lock(&hs->lock);
    
    struct het_queue *queue, *tmp;
    list_for_each_entry_safe(queue, tmp, &hs->queues, list) {
        if (queue->queue_id == queue_id) {
            list_del(&queue->list);
            kfree(queue);
            hs->queue_count--;
            spin_unlock(&hs->lock);
            return 0;
        }
    }
    
    spin_unlock(&hs->lock);
    return -1;
}

int het_flush_queue(struct het_queue *queue) {
    if (queue == NULL) {
        return -1;
    }
    
    /* 等待所有待处理任务完成 */
    while (queue->pending_count > 0 || queue->running_count > 0) {
        /* TODO: 让出 CPU */
    }
    
    return 0;
}

/* ========== 内存管理 ========== */

struct het_memory_region *het_alloc_memory(struct het_scheduler *hs,
                                            uint32_t device_id,
                                            size_t size,
                                            het_mem_type_t type,
                                            uint32_t flags) {
    if (hs == NULL || size == 0) {
        return NULL;
    }
    
    struct het_memory_region *region = kmalloc(sizeof(*region));
    if (region == NULL) {
        return NULL;
    }
    
    memset(region, 0, sizeof(*region));
    
    /* 分配内存 */
    if (type == HET_MEM_SYSTEM || type == HET_MEM_UNIFIED) {
        region->host_ptr = kmalloc(size);
        if (region->host_ptr == NULL) {
            kfree(region);
            return NULL;
        }
        memset(region->host_ptr, 0, size);
    }
    
    region->size = size;
    region->type = type;
    region->flags = flags;
    region->owner_device = device_id;
    region->ref_count = 1;
    INIT_LIST_HEAD(&region->list);
    
    spin_lock(&hs->lock);
    region->id = ++hs->next_mem_id;
    list_add_tail(&region->list, &hs->memory_regions);
    spin_unlock(&hs->lock);
    
    return region;
}

int het_free_memory(struct het_scheduler *hs, struct het_memory_region *region) {
    if (hs == NULL || region == NULL) {
        return -1;
    }
    
    spin_lock(&hs->lock);
    
    if (--region->ref_count > 0) {
        spin_unlock(&hs->lock);
        return 0;  /* 还有引用 */
    }
    
    list_del(&region->list);
    spin_unlock(&hs->lock);
    
    if (region->host_ptr != NULL && region->type != HET_MEM_DEVICE) {
        kfree(region->host_ptr);
    }
    kfree(region);
    
    return 0;
}

int het_memcpy(struct het_scheduler *hs,
               struct het_memory_region *dst, size_t dst_offset,
               struct het_memory_region *src, size_t src_offset,
               size_t size,
               uint64_t *event) {
    if (hs == NULL || dst == NULL || src == NULL) {
        return -1;
    }
    
    if (dst_offset + size > dst->size || src_offset + size > src->size) {
        return -1;
    }
    
    /* 简单实现：主机端拷贝 */
    if (dst->host_ptr != NULL && src->host_ptr != NULL) {
        memcpy((char *)dst->host_ptr + dst_offset,
               (char *)src->host_ptr + src_offset, size);
    }
    
    hs->total_bytes += size;
    
    if (event != NULL) {
        *event = 0;  /* 同步完成，无事件 */
    }
    
    return 0;
}

int het_memfill(struct het_scheduler *hs,
                struct het_memory_region *dst, size_t offset,
                uint32_t pattern, size_t size,
                uint64_t *event) {
    if (hs == NULL || dst == NULL) {
        return -1;
    }
    
    if (offset + size > dst->size) {
        return -1;
    }
    
    /* 简单实现：主机端填充 */
    if (dst->host_ptr != NULL) {
        uint32_t *ptr = (uint32_t *)((char *)dst->host_ptr + offset);
        size_t count = size / sizeof(uint32_t);
        for (size_t i = 0; i < count; i++) {
            ptr[i] = pattern;
        }
    }
    
    if (event != NULL) {
        *event = 0;
    }
    
    return 0;
}

void *het_map_memory(struct het_memory_region *region) {
    if (region == NULL) {
        return NULL;
    }
    return region->host_ptr;
}

int het_unmap_memory(struct het_memory_region *region) {
    (void)region;
    return 0;
}

/* ========== 任务管理 ========== */

struct het_task *het_create_task(struct het_scheduler *hs,
                                  het_task_type_t type,
                                  const char *name) {
    if (hs == NULL) {
        return NULL;
    }
    
    struct het_task *task = kmalloc(sizeof(*task));
    if (task == NULL) {
        return NULL;
    }
    
    memset(task, 0, sizeof(*task));
    
    task->type = type;
    task->state = HET_TASK_CREATED;
    
    if (name != NULL) {
        strncpy(task->name, name, HET_MAX_NAME_LEN - 1);
    }
    
    INIT_LIST_HEAD(&task->list);
    
    spin_lock(&hs->lock);
    task->task_id = ++hs->next_task_id;
    list_add_tail(&task->list, &hs->all_tasks);
    hs->total_tasks++;
    spin_unlock(&hs->lock);
    
    return task;
}

int het_destroy_task(struct het_task *task) {
    if (task == NULL) {
        return -1;
    }
    
    if (task->args != NULL) {
        kfree(task->args);
    }
    
    kfree(task);
    return 0;
}

int het_set_kernel_args(struct het_task *task, void *args, size_t size) {
    if (task == NULL) {
        return -1;
    }
    
    if (task->args != NULL) {
        kfree(task->args);
    }
    
    if (args != NULL && size > 0) {
        task->args = kmalloc(size);
        if (task->args == NULL) {
            return -1;
        }
        memcpy(task->args, args, size);
        task->args_size = size;
    }
    
    return 0;
}

int het_set_ndrange(struct het_task *task, struct het_ndrange *ndrange) {
    if (task == NULL || ndrange == NULL) {
        return -1;
    }
    
    memcpy(&task->ndrange, ndrange, sizeof(*ndrange));
    return 0;
}

int het_add_dependency(struct het_task *task, uint64_t event_id) {
    if (task == NULL) {
        return -1;
    }
    
    if (task->dep_count >= HET_MAX_DEPENDENCIES) {
        return -1;
    }
    
    task->dependencies[task->dep_count++] = event_id;
    return 0;
}

int het_add_memory_region(struct het_task *task, struct het_memory_region *region) {
    if (task == NULL || region == NULL) {
        return -1;
    }
    
    if (task->mem_count >= 16) {
        return -1;
    }
    
    task->mem_regions[task->mem_count++] = region;
    region->ref_count++;
    
    return 0;
}

int het_submit_task(struct het_scheduler *hs,
                    struct het_task *task,
                    uint32_t queue_id,
                    uint64_t *event) {
    if (hs == NULL || task == NULL) {
        return -1;
    }
    
    /* 查找队列 */
    struct het_queue *queue = NULL;
    spin_lock(&hs->lock);
    list_for_each_entry(queue, &hs->queues, list) {
        if (queue->queue_id == queue_id) {
            break;
        }
    }
    spin_unlock(&hs->lock);
    
    if (queue == NULL) {
        return -1;
    }
    
    /* 创建事件 */
    struct het_event *evt = kmalloc(sizeof(*evt));
    if (evt == NULL) {
        return -1;
    }
    
    memset(evt, 0, sizeof(*evt));
    INIT_LIST_HEAD(&evt->waiters);
    INIT_LIST_HEAD(&evt->list);
    
    spin_lock(&hs->lock);
    evt->event_id = ++hs->next_event_id;
    evt->task_id = task->task_id;
    list_add_tail(&evt->list, &hs->events);
    spin_unlock(&hs->lock);
    
    task->event_id = evt->event_id;
    task->queue_id = queue_id;
    task->state = HET_TASK_SUBMITTED;
    task->submit_time = 0;  /* TODO */
    
    /* 添加到队列 */
    spin_lock(&hs->lock);
    list_add_tail(&task->list, &queue->pending);
    queue->pending_count++;
    spin_unlock(&hs->lock);
    
    /* 立即执行 (简化实现) */
    task->state = HET_TASK_RUNNING;
    task->start_time = 0;
    
    /* 模拟执行 */
    task->result = 0;
    task->state = HET_TASK_COMPLETED;
    task->end_time = 0;
    
    /* 标记事件完成 */
    evt->completed = 1;
    evt->status = 0;
    
    /* 从队列移除 */
    spin_lock(&hs->lock);
    list_del(&task->list);
    queue->pending_count--;
    hs->completed_tasks++;
    spin_unlock(&hs->lock);
    
    if (event != NULL) {
        *event = evt->event_id;
    }
    
    return 0;
}

/* ========== 事件管理 ========== */

int het_wait_event(struct het_scheduler *hs, uint64_t event_id, uint64_t timeout) {
    (void)timeout;
    
    if (hs == NULL) {
        return -1;
    }
    
    spin_lock(&hs->lock);
    
    struct het_event *evt;
    list_for_each_entry(evt, &hs->events, list) {
        if (evt->event_id == event_id) {
            spin_unlock(&hs->lock);
            return evt->completed ? 0 : -1;
        }
    }
    
    spin_unlock(&hs->lock);
    return -1;
}

int het_wait_events(struct het_scheduler *hs, uint64_t *events, uint32_t count,
                    uint64_t timeout) {
    if (hs == NULL || events == NULL || count == 0) {
        return -1;
    }
    
    for (uint32_t i = 0; i < count; i++) {
        if (het_wait_event(hs, events[i], timeout) != 0) {
            return -1;
        }
    }
    
    return 0;
}

int het_set_callback(struct het_scheduler *hs, uint64_t event_id,
                     void (*callback)(struct het_event *, void *),
                     void *data) {
    if (hs == NULL) {
        return -1;
    }
    
    spin_lock(&hs->lock);
    
    struct het_event *evt;
    list_for_each_entry(evt, &hs->events, list) {
        if (evt->event_id == event_id) {
            evt->callback = callback;
            evt->callback_data = data;
            
            /* 如果已完成，立即调用回调 */
            if (evt->completed && callback != NULL) {
                callback(evt, data);
            }
            
            spin_unlock(&hs->lock);
            return 0;
        }
    }
    
    spin_unlock(&hs->lock);
    return -1;
}

int het_query_event(struct het_scheduler *hs, uint64_t event_id) {
    if (hs == NULL) {
        return -1;
    }
    
    spin_lock(&hs->lock);
    
    struct het_event *evt;
    list_for_each_entry(evt, &hs->events, list) {
        if (evt->event_id == event_id) {
            int status = evt->completed ? 1 : 0;
            spin_unlock(&hs->lock);
            return status;
        }
    }
    
    spin_unlock(&hs->lock);
    return -1;
}

/* ========== 自动设备选择 ========== */

uint32_t het_select_device(struct het_scheduler *hs,
                            struct het_perf_model *model) {
    if (hs == NULL) {
        return 0;
    }
    
    int64_t best_score = -1;
    uint32_t best_device = 0;
    
    spin_lock(&hs->lock);
    
    struct het_device *dev;
    list_for_each_entry(dev, &hs->devices, list) {
        if (!dev->is_available) continue;
        
        int64_t score = het_compute_score(dev, model);
        model->scores[dev->device_id] = score;
        
        if (score > best_score) {
            best_score = score;
            best_device = dev->device_id;
        }
    }
    
    spin_unlock(&hs->lock);
    
    return best_device;
}

uint64_t het_estimate_time(struct het_device *dev,
                            struct het_perf_model *model) {
    if (dev == NULL || model == NULL) {
        return 0;
    }
    
    /* 估算执行时间 = 计算时间 + 内存传输时间 */
    uint64_t compute_time = model->flops_estimate / 
                            (dev->perf.gflops * 1000000000ULL / 1000);
    
    uint64_t memory_time = model->bytes_estimate / 
                           (dev->perf.bandwidth * 1024 * 1024ULL / 1000);
    
    return compute_time + memory_time + dev->perf.latency;
}

int64_t het_compute_score(struct het_device *dev,
                          struct het_perf_model *model) {
    if (dev == NULL || model == NULL) {
        return 0;
    }
    
    /* 得分 = 计算能力 * 计算权重 + 内存带宽 * 内存权重 - 延迟 * 延迟权重 */
    /* 所有计算使用定点数（精度 1000） */
    int64_t compute_score = (int64_t)dev->perf.gflops * model->compute_weight;
    int64_t memory_score = (int64_t)(dev->perf.bandwidth / 1000) * model->memory_weight;
    int64_t latency_penalty = (int64_t)(dev->perf.latency / 1000) * model->latency_weight;
    
    /* 考虑负载 (load 0-100) */
    int64_t load_factor = 1000 - (int64_t)dev->load * 10;  /* 1000 - load*10 */
    if (load_factor < 0) load_factor = 0;
    
    /* 最终得分 */
    int64_t base_score = compute_score + memory_score - latency_penalty;
    int64_t final_score = (base_score * load_factor) / HET_FIXED_SCALE;
    
    return final_score;
}

/* ========== 便捷 API ========== */

int het_compute(struct het_scheduler *hs,
                uint64_t flops,
                void *input, size_t input_size,
                void *output, size_t output_size,
                uint64_t *event) {
    if (hs == NULL) {
        return -1;
    }
    
    /* 创建性能模型 */
    struct het_perf_model model;
    memset(&model, 0, sizeof(model));
    model.flops_estimate = flops;
    model.bytes_estimate = input_size + output_size;
    
    /* 选择最优设备 */
    uint32_t device_id = het_select_device(hs, &model);
    
    /* 创建任务 */
    struct het_task *task = het_create_task(hs, HET_TASK_COMPUTE, "compute");
    if (task == NULL) {
        return -1;
    }
    
    /* 分配内存区域 */
    struct het_memory_region *in_region = NULL;
    struct het_memory_region *out_region = NULL;
    
    if (input != NULL && input_size > 0) {
        in_region = het_alloc_memory(hs, device_id, input_size, 
                                      HET_MEM_SYSTEM, HET_MEM_FLAG_READ);
        if (in_region == NULL) {
            het_destroy_task(task);
            return -1;
        }
        memcpy(in_region->host_ptr, input, input_size);
        het_add_memory_region(task, in_region);
    }
    
    if (output != NULL && output_size > 0) {
        out_region = het_alloc_memory(hs, device_id, output_size,
                                       HET_MEM_SYSTEM, HET_MEM_FLAG_WRITE);
        if (out_region == NULL) {
            if (in_region != NULL) het_free_memory(hs, in_region);
            het_destroy_task(task);
            return -1;
        }
        het_add_memory_region(task, out_region);
    }
    
    /* 提交任务 */
    int ret = het_submit_task(hs, task, 0, event);
    
    /* 拷贝输出 */
    if (ret == 0 && out_region != NULL && output != NULL) {
        memcpy(output, out_region->host_ptr, output_size);
    }
    
    /* 清理 */
    if (in_region != NULL) het_free_memory(hs, in_region);
    if (out_region != NULL) het_free_memory(hs, out_region);
    
    return ret;
}

int het_matmul(struct het_scheduler *hs,
               void *A, void *B, void *C,
               uint32_t M, uint32_t N, uint32_t K,
               uint64_t *event) {
    if (hs == NULL || A == NULL || B == NULL || C == NULL) {
        return -1;
    }
    
    /* 估算 FLOPS: 2 * M * N * K */
    uint64_t flops = 2ULL * M * N * K;
    uint64_t bytes = (M * K + K * N + M * N) * sizeof(float);
    
    return het_compute(hs, flops, NULL, bytes, NULL, bytes, event);
}

int het_conv2d(struct het_scheduler *hs,
               void *input, void *kernel, void *output,
               uint32_t in_h, uint32_t in_w, uint32_t in_c,
               uint32_t k_h, uint32_t k_w, uint32_t k_c,
               uint64_t *event) {
    if (hs == NULL) {
        return -1;
    }
    
    /* 估算 FLOPS */
    uint64_t out_h = in_h - k_h + 1;
    uint64_t out_w = in_w - k_w + 1;
    uint64_t flops = 2ULL * out_h * out_w * k_h * k_w * in_c * k_c;
    
    uint64_t bytes = (in_h * in_w * in_c + k_h * k_w * in_c * k_c + 
                      out_h * out_w * k_c) * sizeof(float);
    
    return het_compute(hs, flops, NULL, bytes, NULL, bytes, event);
}

/* ========== 统计 ========== */

void het_get_stats(struct het_scheduler *hs,
                   uint64_t *tasks, uint64_t *completed,
                   uint64_t *bytes) {
    if (hs == NULL) {
        return;
    }
    
    spin_lock(&hs->lock);
    
    if (tasks != NULL) *tasks = hs->total_tasks;
    if (completed != NULL) *completed = hs->completed_tasks;
    if (bytes != NULL) *bytes = hs->total_bytes;
    
    spin_unlock(&hs->lock);
}

void het_print_devices(struct het_scheduler *hs) {
    if (hs == NULL) {
        return;
    }
    
    println("=== Heterogeneous Devices ===");
    
    spin_lock(&hs->lock);
    
    struct het_device *dev;
    list_for_each_entry(dev, &hs->devices, list) {
        print("  [");
        print_dec(dev->device_id);
        print("] ");
        print(dev->name);
        print(" (");
        print((char *)het_dev_type_name(dev->type));
        println(")");
        
        print("      Compute Units: ");
        print_dec(dev->compute_units);
        println("");
        
        print("      Memory: ");
        print_dec(dev->global_mem_size / (1024 * 1024));
        println(" MB");
        
        print("      Performance: ");
        print_dec(dev->perf.gflops);
        println(" GFLOPS");
    }
    
    spin_unlock(&hs->lock);
}

void het_print_tasks(struct het_scheduler *hs) {
    if (hs == NULL) {
        return;
    }
    
    println("=== Heterogeneous Tasks ===");
    
    spin_lock(&hs->lock);
    
    print("  Total: ");
    print_dec(hs->total_tasks);
    print(", Completed: ");
    print_dec(hs->completed_tasks);
    print(", Failed: ");
    print_dec(hs->failed_tasks);
    println("");
    
    spin_unlock(&hs->lock);
}

/* ========== 全局调度器 ========== */

struct het_scheduler *het_get_global(void) {
    return &global_hs;
}

int het_global_init(void) {
    int ret = het_init(&global_hs);
    
    if (ret == 0) {
        /* 注册 CPU 设备 */
        struct het_device *cpu_dev = kmalloc(sizeof(*cpu_dev));
        if (cpu_dev != NULL) {
            memset(cpu_dev, 0, sizeof(*cpu_dev));
            strncpy(cpu_dev->name, "CPU", HET_MAX_NAME_LEN - 1);
            cpu_dev->type = HET_DEV_CPU;
            cpu_dev->capabilities = HET_CAP_COMPUTE | HET_CAP_FP64 | HET_CAP_INT8;
            cpu_dev->compute_units = 1;  /* TODO: 获取实际核心数 */
            cpu_dev->clock_freq = 3000000000ULL;
            cpu_dev->global_mem_size = 8ULL * 1024 * 1024 * 1024;  /* 8GB */
            cpu_dev->perf.gflops = 100;
            cpu_dev->perf.bandwidth = 50000;  /* 50 GB/s */
            cpu_dev->perf.latency = 100;  /* 100 ns */
            cpu_dev->perf.efficiency = 0.8;
            
            het_register_device(&global_hs, cpu_dev);
        }
    }
    
    return ret;
}
