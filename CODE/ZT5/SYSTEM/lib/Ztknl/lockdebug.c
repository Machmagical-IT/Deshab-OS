/*
 * Zratro OS - Lock Debugging Implementation
 * 
 * 内核锁调试和死锁检测
 */

#include "lockdebug.h"
#include "console.h"
#include "smp.h"
#include "heap.h"
#include "string.h"

/* 锁调试配置 */
#define LOCK_DEBUG_ENABLED      1
#define MAX_LOCK_HISTORY        256
#define MAX_LOCK_NAME           32
#define DEADLOCK_THRESHOLD      5000000     /* 5 秒超时 */

/* 锁状态 */
typedef enum {
    LOCK_STATE_FREE,
    LOCK_STATE_HELD,
    LOCK_STATE_CONTENDED
} lock_state_t;

/* 锁信息 */
struct lock_info {
    char name[MAX_LOCK_NAME];
    lock_type_t type;
    lock_state_t state;
    volatile void *lock_addr;
    
    /* 持有者信息 */
    int owner_cpu;
    uint64_t owner_task;
    uint64_t acquire_time;
    uint64_t hold_count;
    uint64_t wait_count;
    uint64_t contentions;
    uint64_t max_hold_time;
    
    /* 等待者链表 */
    int waiters[MAX_CPUS];
    int waiter_count;
    
    /* 链表 */
    struct lock_info *next;
};

/* 锁历史记录 */
struct lock_history_entry {
    volatile void *lock_addr;
    char name[MAX_LOCK_NAME];
    int cpu;
    uint64_t task;
    uint64_t timestamp;
    int action;     /* 0 = acquire, 1 = release, 2 = wait, 3 = timeout */
};

/* 全局变量 */
static struct lock_info *lock_list = NULL;
static volatile int lock_debug_lock = 0;
static int lock_debug_enabled = LOCK_DEBUG_ENABLED;

static struct lock_history_entry lock_history[MAX_LOCK_HISTORY];
static int history_head = 0;
static int history_count = 0;

/* 统计 */
static uint64_t total_acquires = 0;
static uint64_t total_releases = 0;
static uint64_t total_contentions = 0;
static uint64_t total_timeouts = 0;
static uint64_t total_deadlocks = 0;

/* ========== 初始化 ========== */

int lockdebug_init(void) {
    println("[LOCKDEBUG] Initializing lock debugging...");
    
    lock_list = NULL;
    lock_debug_lock = 0;
    lock_debug_enabled = LOCK_DEBUG_ENABLED;
    history_head = 0;
    history_count = 0;
    
    println("[LOCKDEBUG] Initialized successfully");
    return 0;
}

/* ========== 锁注册 ========== */

/* 注册锁 */
int lockdebug_register(volatile void *lock, const char *name, lock_type_t type) {
    if (!lock_debug_enabled || lock == NULL || name == NULL) {
        return -1;
    }
    
    /* 分配锁信息 */
    struct lock_info *info = kmalloc(sizeof(struct lock_info));
    if (info == NULL) {
        return -1;
    }
    
    /* 初始化 */
    memset(info, 0, sizeof(struct lock_info));
    strncpy(info->name, name, MAX_LOCK_NAME - 1);
    info->type = type;
    info->state = LOCK_STATE_FREE;
    info->lock_addr = lock;
    
    /* 加入链表 */
    while (__sync_lock_test_and_set(&lock_debug_lock, 1)) {
        asm volatile("pause");
    }
    
    info->next = lock_list;
    lock_list = info;
    
    __sync_lock_release(&lock_debug_lock);
    
    return 0;
}

/* 注销锁 */
int lockdebug_unregister(volatile void *lock) {
    if (!lock_debug_enabled || lock == NULL) {
        return -1;
    }
    
    while (__sync_lock_test_and_set(&lock_debug_lock, 1)) {
        asm volatile("pause");
    }
    
    struct lock_info *prev = NULL;
    struct lock_info *curr = lock_list;
    
    while (curr != NULL) {
        if (curr->lock_addr == lock) {
            if (prev != NULL) {
                prev->next = curr->next;
            } else {
                lock_list = curr->next;
            }
            kfree(curr);
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    
    __sync_lock_release(&lock_debug_lock);
    return 0;
}

/* ========== 锁事件记录 ========== */

/* 查找锁信息 */
static struct lock_info *find_lock(volatile void *lock) {
    struct lock_info *info = lock_list;
    
    while (info != NULL) {
        if (info->lock_addr == lock) {
            return info;
        }
        info = info->next;
    }
    
    return NULL;
}

/* 添加历史记录 */
static void add_history(volatile void *lock, const char *name, int action) {
    struct lock_history_entry *entry = &lock_history[history_head];
    
    entry->lock_addr = lock;
    strncpy(entry->name, name, MAX_LOCK_NAME - 1);
    entry->cpu = smp_processor_id();
    entry->task = 0;    /* TODO: 获取当前任务 ID */
    entry->timestamp = get_tick_count();
    entry->action = action;
    
    history_head = (history_head + 1) % MAX_LOCK_HISTORY;
    if (history_count < MAX_LOCK_HISTORY) {
        history_count++;
    }
}

/* 记录锁获取 */
void lockdebug_acquire(volatile void *lock, const char *name, int type) {
    if (!lock_debug_enabled) return;
    
    total_acquires++;
    
    while (__sync_lock_test_and_set(&lock_debug_lock, 1)) {
        asm volatile("pause");
    }
    
    struct lock_info *info = find_lock(lock);
    
    if (info == NULL) {
        /* 自动注册新锁 */
        __sync_lock_release(&lock_debug_lock);
        lockdebug_register(lock, name, type);
        while (__sync_lock_test_and_set(&lock_debug_lock, 1)) {
            asm volatile("pause");
        }
        info = find_lock(lock);
    }
    
    if (info != NULL) {
        info->state = LOCK_STATE_HELD;
        info->owner_cpu = smp_processor_id();
        info->owner_task = 0;   /* TODO */
        info->acquire_time = get_tick_count();
        info->hold_count++;
        
        add_history(lock, name, 0);
    }
    
    __sync_lock_release(&lock_debug_lock);
}

/* 记录锁释放 */
void lockdebug_release(volatile void *lock) {
    if (!lock_debug_enabled) return;
    
    total_releases++;
    
    while (__sync_lock_test_and_set(&lock_debug_lock, 1)) {
        asm volatile("pause");
    }
    
    struct lock_info *info = find_lock(lock);
    
    if (info != NULL) {
        uint64_t hold_time = get_tick_count() - info->acquire_time;
        
        if (hold_time > info->max_hold_time) {
            info->max_hold_time = hold_time;
        }
        
        info->state = LOCK_STATE_FREE;
        info->owner_cpu = -1;
        info->owner_task = 0;
        
        add_history(lock, info->name, 1);
    }
    
    __sync_lock_release(&lock_debug_lock);
}

/* 记录锁等待 */
void lockdebug_wait(volatile void *lock, const char *name) {
    if (!lock_debug_enabled) return;
    
    total_contentions++;
    
    while (__sync_lock_test_and_set(&lock_debug_lock, 1)) {
        asm volatile("pause");
    }
    
    struct lock_info *info = find_lock(lock);
    
    if (info != NULL) {
        info->state = LOCK_STATE_CONTENDED;
        info->wait_count++;
        
        int cpu = smp_processor_id();
        if (info->waiter_count < MAX_CPUS) {
            info->waiters[info->waiter_count++] = cpu;
        }
        
        add_history(lock, name, 2);
    }
    
    __sync_lock_release(&lock_debug_lock);
}

/* ========== 死锁检测 ========== */

/* 检查死锁 */
int lockdebug_check_deadlock(void) {
    if (!lock_debug_enabled) return 0;
    
    uint64_t now = get_tick_count();
    int deadlocks = 0;
    
    while (__sync_lock_test_and_set(&lock_debug_lock, 1)) {
        asm volatile("pause");
    }
    
    struct lock_info *info = lock_list;
    
    while (info != NULL) {
        if (info->state == LOCK_STATE_HELD || info->state == LOCK_STATE_CONTENDED) {
            uint64_t hold_time = now - info->acquire_time;
            
            if (hold_time > DEADLOCK_THRESHOLD) {
                println("");
                println("=== DEADLOCK DETECTED ===");
                
                print("Lock: ");
                println(info->name);
                
                print("Type: ");
                switch (info->type) {
                    case LOCK_TYPE_SPINLOCK: println("spinlock"); break;
                    case LOCK_TYPE_MUTEX: println("mutex"); break;
                    case LOCK_TYPE_RWLOCK: println("rwlock"); break;
                    case LOCK_TYPE_SEMAPHORE: println("semaphore"); break;
                }
                
                print("Owner CPU: ");
                print_dec(info->owner_cpu);
                println("");
                
                print("Hold time: ");
                print_dec(hold_time / 1000000);
                println(" ms");
                
                print("Waiters: ");
                print_dec(info->waiter_count);
                println("");
                
                deadlocks++;
                total_deadlocks++;
            }
        }
        info = info->next;
    }
    
    __sync_lock_release(&lock_debug_lock);
    
    return deadlocks;
}

/* 验证锁状态 */
int lockdebug_validate(volatile void *lock) {
    if (!lock_debug_enabled) return 0;
    
    while (__sync_lock_test_and_set(&lock_debug_lock, 1)) {
        asm volatile("pause");
    }
    
    struct lock_info *info = find_lock(lock);
    int valid = 0;
    
    if (info != NULL) {
        /* 检查锁是否在合理状态 */
        valid = 1;
    }
    
    __sync_lock_release(&lock_debug_lock);
    return valid;
}

/* ========== 统计输出 ========== */

void lockdebug_stats(void) {
    println("=== Lock Debug Statistics ===");
    
    print("Total acquires: ");
    print_dec(total_acquires);
    println("");
    
    print("Total releases: ");
    print_dec(total_releases);
    println("");
    
    print("Contentions: ");
    print_dec(total_contentions);
    println("");
    
    print("Timeouts: ");
    print_dec(total_timeouts);
    println("");
    
    print("Deadlocks detected: ");
    print_dec(total_deadlocks);
    println("");
    
    println("");
    println("=== Active Locks ===");
    
    while (__sync_lock_test_and_set(&lock_debug_lock, 1)) {
        asm volatile("pause");
    }
    
    struct lock_info *info = lock_list;
    int count = 0;
    
    while (info != NULL && count < 10) {
        print(info->name);
        print(": holds=");
        print_dec(info->hold_count);
        print(" waits=");
        print_dec(info->wait_count);
        print(" contentions=");
        print_dec(info->contentions);
        print(" max_hold=");
        print_dec(info->max_hold_time / 1000);
        println(" us");
        
        info = info->next;
        count++;
    }
    
    __sync_lock_release(&lock_debug_lock);
}

/* 打印锁历史 */
void lockdebug_history(void) {
    println("=== Lock History ===");
    
    int start = (history_head - history_count + MAX_LOCK_HISTORY) % MAX_LOCK_HISTORY;
    
    for (int i = 0; i < history_count && i < 20; i++) {
        int idx = (start + i) % MAX_LOCK_HISTORY;
        struct lock_history_entry *entry = &lock_history[idx];
        
        print(entry->name);
        print(" @ CPU");
        print_dec(entry->cpu);
        print(" ");
        
        switch (entry->action) {
            case 0: print("ACQUIRE"); break;
            case 1: print("RELEASE"); break;
            case 2: print("WAIT"); break;
            case 3: print("TIMEOUT"); break;
        }
        
        print(" ");
        print_dec(entry->timestamp / 1000);
        println(" us");
    }
}

/* ========== 控制接口 ========== */

/* 启用锁调试 */
void lockdebug_enable(void) {
    lock_debug_enabled = 1;
    println("[LOCKDEBUG] Enabled");
}

/* 禁用锁调试 */
void lockdebug_disable(void) {
    lock_debug_enabled = 0;
    println("[LOCKDEBUG] Disabled");
}

/* 检查是否启用 */
int lockdebug_is_enabled(void) {
    return lock_debug_enabled;
}
