/*
 * Zratro OS - OPU (Open Processing Unit) Implementation
 * 
 * 用于GPU、NPU、FPGA等异构处理单元的支持
 */

#include "opu.h"
#include "console.h"
#include "heap.h"
#include "string.h"
#include "smp.h"  /* 用于CPU相关功能 */

/* ========== 全局 OPU 数据 ========== */

__attribute__((aligned(4096))) struct opu_local opu_data[MAX_OPUS];
int opu_count = 0;

/* ========== OPU 初始化 ========== */

int opu_init(void) {
    memset(opu_data, 0, sizeof(opu_data));
    
    uint64_t opu_data_addr = (uint64_t)&opu_data[0];
    
    if (opu_data_addr == 0) {
        console_error("opu_data address is NULL");
        for (;;) asm volatile("hlt");
    }
    
    if (opu_data_addr < 0xFFFF800000000000ULL) {
        console_error("opu_data address not in Higher Half");
        for (;;) asm volatile("hlt");
    }
    
    if (opu_data_addr & 0xFFF) {
        console_error("opu_data address not page-aligned");
        for (;;) asm volatile("hlt");
    }
    
    for (int i = 0; i < MAX_OPUS; i++) {
        opu_data[i].opu_id = i;
        opu_data[i].device_id = 0;
        opu_data[i].state = OPU_STATE_OFFLINE;
        
        uint32_t tsc_lo, tsc_hi;
        asm volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
        uint64_t tsc = ((uint64_t)tsc_hi << 32) | tsc_lo;
        opu_data[i].stack_canary = tsc ^ (tsc >> 32) ^ 0xDEADBEEFCAFEBABEULL ^ 
                                   ((uint64_t)i * 0x9E3779B97F4A7C15ULL);
        
        opu_data[i].command_queue = NULL;
        opu_data[i].queue_head = 0;
        opu_data[i].queue_tail = 0;
        opu_data[i].queue_size = 0;
        
        mutex_init(&opu_data[i].queue_mutex);
        spinlock_init(&opu_data[i].state_lock);
        
        opu_data[i].compute_units_used = 0;
        opu_data[i].memory_bandwidth = 0;
        opu_data[i].total_compute_time = 0;
    }
    
    return 0;
}

/* ========== OPU 设备注册 ========== */

int opu_register_device(uint32_t device_id, uint32_t opu_type) {
    if (opu_count >= MAX_OPUS) {
        return -1;
    }
    
    int opu_id = opu_count++;
    
    opu_data[opu_id].device_id = device_id;
    opu_data[opu_id].state = OPU_STATE_ONLINE;
    print_dec(opu_id);
    println("");
    
    return opu_id;
}

/* ========== OPU 任务提交 ========== */

int opu_submit_task(uint32_t opu_id, void *task_desc) {
    if (opu_id >= MAX_OPUS || task_desc == NULL) {
        return -1;
    }
    
    if (opu_data[opu_id].state != OPU_STATE_ONLINE && 
        opu_data[opu_id].state != OPU_STATE_RUNNING) {
        return -1;
    }
    
    mutex_lock(&opu_data[opu_id].queue_mutex);
    
    /* 如果队列未初始化，分配内存 */
    if (opu_data[opu_id].command_queue == NULL) {
        opu_data[opu_id].command_queue = kmalloc(OPU_QUEUE_SIZE * sizeof(uint64_t));
        if (opu_data[opu_id].command_queue == NULL) {
            mutex_unlock(&opu_data[opu_id].queue_mutex);
            return -1;
        }
        opu_data[opu_id].queue_size = OPU_QUEUE_SIZE;
        opu_data[opu_id].queue_head = 0;
        opu_data[opu_id].queue_tail = 0;
    }
    
    /* 检查队列是否已满 */
    uint32_t next_tail = (opu_data[opu_id].queue_tail + 1) % opu_data[opu_id].queue_size;
    if (next_tail == opu_data[opu_id].queue_head) {
        mutex_unlock(&opu_data[opu_id].queue_mutex);
        return -1;  /* 队列已满 */
    }
    
    /* 添加任务到队列 */
    opu_data[opu_id].command_queue[opu_data[opu_id].queue_tail] = (uint64_t)task_desc;
    opu_data[opu_id].queue_tail = next_tail;
    
    /* 更新状态 */
    opu_set_state(opu_id, OPU_STATE_RUNNING);
    
    mutex_unlock(&opu_data[opu_id].queue_mutex);
    
    return 0;
}

/* ========== OPU 任务等待 ========== */

int opu_wait_task(uint32_t opu_id, uint64_t task_id) {
    if (opu_id >= MAX_OPUS) {
        return -1;
    }
    
    /* 简单实现：轮询等待 */
    while (opu_data[opu_id].state == OPU_STATE_RUNNING) {
        asm volatile("pause");
    }
    
    return 0;
}

/* ========== OPU 状态管理 ========== */

int opu_get_state(uint32_t opu_id) {
    if (opu_id >= MAX_OPUS) {
        return -1;
    }
    
    spin_lock(&opu_data[opu_id].state_lock);
    int state = opu_data[opu_id].state;
    spin_unlock(&opu_data[opu_id].state_lock);
    
    return state;
}

void opu_set_state(uint32_t opu_id, int state) {
    if (opu_id >= MAX_OPUS) {
        return;
    }
    
    spin_lock(&opu_data[opu_id].state_lock);
    opu_data[opu_id].state = state;
    spin_unlock(&opu_data[opu_id].state_lock);
}

/* ========== OPU 内存管理 ========== */

void *opu_alloc_memory(uint32_t opu_id, size_t size, uint32_t flags) {
    if (opu_id >= MAX_OPUS || size == 0) {
        return NULL;
    }
    
    void *ptr = kvalloc(size, 0x1000);
    if (ptr == NULL) {
        return NULL;
    }
    
    if ((uint64_t)ptr & 0xFFF) {
        kfree(ptr);
        return NULL;
    }
    
    return ptr;
}

int opu_free_memory(uint32_t opu_id, void *ptr) {
    if (opu_id >= MAX_OPUS || ptr == NULL) {
        return -1;
    }
    
    if ((uint64_t)ptr & 0xFFF) {
        return -1;
    }
    
    kfree(ptr);
    return 0;
}

/* ========== OPU 调试 ========== */

void opu_print_status(void) {
    println("=== OPU Status ===");
    
    print("OPUs registered: ");
    print_dec(opu_count);
    println("");
    
    for (int i = 0; i < opu_count && i < MAX_OPUS; i++) {
        print("  [OPU ");
        print_dec(i);
        print("] Device: ");
        print_dec(opu_data[i].device_id);
        print(", State: ");
        
        switch (opu_data[i].state) {
            case OPU_STATE_OFFLINE:   print("OFFLINE"); break;
            case OPU_STATE_ONLINE:    print("ONLINE");  break;
            case OPU_STATE_RUNNING:   print("RUNNING"); break;
            case OPU_STATE_SUSPENDED: print("SUSPENDED"); break;
            case OPU_STATE_ERROR:     print("ERROR");   break;
            case OPU_STATE_HALTED:    print("HALTED");  break;
            default:                  print("UNKNOWN"); break;
        }
        
        print(", Tasks: ");
        uint32_t pending = (opu_data[i].queue_tail - opu_data[i].queue_head + 
                           opu_data[i].queue_size) % opu_data[i].queue_size;
        print_dec(pending);
        println("");
    }
}