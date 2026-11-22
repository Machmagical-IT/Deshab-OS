#include "task.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "gdt.h"
#include "console.h"
#include "string.h"
#include "smp.h"
#include "cow.h"
#include "ipc.h"

/* ========== PID 哈希表 (O(1) 查找) ========== */

#define PID_HASH_BITS      8      /* 哈希表大小 = 256 */
#define PID_HASH_SIZE      (1 << PID_HASH_BITS)
#define PID_HASH_MASK      (PID_HASH_SIZE - 1)

/* PID 哈希桶 */
static struct process *pid_hash_table[PID_HASH_SIZE];
static volatile int pid_hash_lock = 0;

/* 哈希函数 */
static inline uint32_t pid_hash(uint32_t pid) {
    /* 使用乘法哈希，分布更均匀 */
    uint32_t hash = pid * 2654435761U;  /* 黄金比例常数 */
    return hash & PID_HASH_MASK;
}

/* 插入 PID 到哈希表 */
static void pid_hash_insert(struct process *proc) {
    if (proc == NULL) return;
    
    uint32_t bucket = pid_hash(proc->pid);
    
    spin_lock(&pid_hash_lock);
    proc->hash_next = pid_hash_table[bucket];
    if (proc->hash_next != NULL) {
        proc->hash_next->hash_prev = proc;
    }
    pid_hash_table[bucket] = proc;
    proc->hash_prev = NULL;
    spin_unlock(&pid_hash_lock);
}

/* 从哈希表移除 PID */
static void pid_hash_remove(struct process *proc) {
    if (proc == NULL) return;
    
    spin_lock(&pid_hash_lock);
    
    if (proc->hash_prev != NULL) {
        proc->hash_prev->hash_next = proc->hash_next;
    } else {
        uint32_t bucket = pid_hash(proc->pid);
        pid_hash_table[bucket] = proc->hash_next;
    }
    
    if (proc->hash_next != NULL) {
        proc->hash_next->hash_prev = proc->hash_prev;
    }
    
    proc->hash_prev = NULL;
    proc->hash_next = NULL;
    
    spin_unlock(&pid_hash_lock);
}

/* 哈希表查找 PID (O(1) 平均) */
static struct process *pid_hash_lookup(uint32_t pid) {
    uint32_t bucket = pid_hash(pid);
    
    spin_lock(&pid_hash_lock);
    
    struct process *proc = pid_hash_table[bucket];
    while (proc != NULL) {
        if (proc->pid == pid) {
            spin_unlock(&pid_hash_lock);
            return proc;
        }
        proc = proc->hash_next;
    }
    
    spin_unlock(&pid_hash_lock);
    return NULL;
}

/* ========== 进程表管理 ========== */

/* 进程表 */
static struct process process_table[MAX_PROCESSES];
static uint32_t next_pid = 1;

/* 线程表 (全局可见，供调度器访问) */
struct thread thread_table[MAX_THREADS];
static uint32_t next_tid = 1;

/* 当前运行的线程 */
struct thread *current_thread = NULL;

/* tick 计数 */
static uint64_t tick_count = 0;

/* 内核进程 (idle) */
static struct process *kernel_process = NULL;

/* 内核栈大小 */
#define KERNEL_STACK_SIZE   (64 * 1024)  /* 64 KB */
#define USER_STACK_SIZE     (128 * 1024) /* 128 KB */

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：内核栈缓存池 (Kernel Stack Cache Pool)
 * 
 * 设计理念：
 * - 预分配一组内核栈，减少频繁分配开销
 * - Per-CPU 缓存，无锁快速路径
 * - 延迟释放，避免热点路径内存操作
 * ═══════════════════════════════════════════════════════════════════════════ */

#define KSTACK_CACHE_SIZE   16      /* 每个CPU缓存16个栈 */
#define KSTACK_BATCH_ALLOC  8       /* 批量分配数量 */
#define KSTACK_BATCH_FREE   8       /* 批量释放阈值 */

struct kstack_cache {
    uint64_t stacks[KSTACK_CACHE_SIZE];  /* 栈顶地址 */
    uint32_t count;                       /* 当前缓存数量 */
    uint32_t pad;                         /* 对齐 */
} __attribute__((aligned(64)));

static struct kstack_cache kstack_caches[MAX_CPUS];
static volatile int kstack_global_lock = 0;

/* 待释放栈列表 (延迟释放) */
#define KSTACK_DELAYED_FREE_MAX  64
static uint64_t kstack_delayed_free[KSTACK_DELAYED_FREE_MAX];
static uint32_t kstack_delayed_count = 0;

/* 内核栈分配状态 */
static uint64_t stack_top = 0xFFFFFFFF90000000ULL;

/* 初始化内核栈缓存 */
static void kstack_cache_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        kstack_caches[i].count = 0;
        for (int j = 0; j < KSTACK_CACHE_SIZE; j++) {
            kstack_caches[i].stacks[j] = 0;
        }
    }
    kstack_delayed_count = 0;
}

/* 从缓存快速分配 (无锁) */
static uint64_t kstack_cache_alloc_fast(void) {
    int cpu = smp_processor_id();
    struct kstack_cache *cache = &kstack_caches[cpu];
    
    if (cache->count > 0) {
        return cache->stacks[--cache->count];
    }
    return 0;  /* 缓存为空 */
}

/* 归还到缓存 (无锁) */
static int kstack_cache_free_fast(uint64_t stack_top) {
    int cpu = smp_processor_id();
    struct kstack_cache *cache = &kstack_caches[cpu];
    
    if (cache->count < KSTACK_CACHE_SIZE) {
        cache->stacks[cache->count++] = stack_top;
        return 0;  /* 成功放入缓存 */
    }
    return -1;  /* 缓存已满 */
}

/* 批量填充缓存 */
static void kstack_cache_refill(struct kstack_cache *cache) {
    while (__sync_lock_test_and_set(&kstack_global_lock, 1)) {
        asm volatile("pause");
    }
    
    /* 分配一批栈 */
    uint32_t to_alloc = KSTACK_BATCH_ALLOC;
    if (to_alloc > KSTACK_CACHE_SIZE - cache->count) {
        to_alloc = KSTACK_CACHE_SIZE - cache->count;
    }
    
    for (uint32_t i = 0; i < to_alloc; i++) {
        size_t pages = KERNEL_STACK_SIZE / PAGE_SIZE;
        uint64_t phys = pmm_alloc_pages(pages);
        if (phys == 0) break;
        
        uint64_t virt = stack_top;
        if (vmm_map_range(vmm_get_kernel_space(), virt, phys, pages, VMM_FLAGS_KERNEL) != 0) {
            pmm_free_pages(phys, pages);
            break;
        }
        
        stack_top += pages * PAGE_SIZE;
        
        /* 清零栈空间 */
        uint8_t *stack_mem = (uint8_t *)virt;
        for (size_t j = 0; j < pages * PAGE_SIZE; j++) {
            stack_mem[j] = 0;
        }
        
        cache->stacks[cache->count++] = virt + pages * PAGE_SIZE;
    }
    
    __sync_lock_release(&kstack_global_lock);
}

/* 分配内核栈 (带缓存) */
static uint64_t alloc_kernel_stack(size_t size) {
    (void)size;  /* 使用固定的 KERNEL_STACK_SIZE */
    
    /* 快速路径：从 Per-CPU 缓存分配 */
    uint64_t stack = kstack_cache_alloc_fast();
    if (stack != 0) {
        return stack;
    }
    
    /* 缓存为空，批量填充 */
    int cpu = smp_processor_id();
    struct kstack_cache *cache = &kstack_caches[cpu];
    kstack_cache_refill(cache);
    
    /* 再次尝试 */
    if (cache->count > 0) {
        return cache->stacks[--cache->count];
    }
    
    /* 直接分配（慢路径） */
    size_t pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    if (pages == 0) {
        // println("[TASK] alloc_kernel_stack: Invalid size");
        return 0;
    }
    
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        // println("[TASK] alloc_kernel_stack: Failed to allocate physical pages");
        return 0;
    }
    
    /* 映射到内核地址空间 */
    uint64_t virt = stack_top;
    
    while (__sync_lock_test_and_set(&kstack_global_lock, 1)) {
        asm volatile("pause");
    }
    
    if (vmm_map_range(vmm_get_kernel_space(), virt, phys, pages, VMM_FLAGS_KERNEL) != 0) {
        // println("[TASK] alloc_kernel_stack: Failed to map virtual range");
        pmm_free_pages(phys, pages);
        __sync_lock_release(&kstack_global_lock);
        return 0;
    }
    
    stack_top += pages * PAGE_SIZE;
    __sync_lock_release(&kstack_global_lock);
    
    /* 初始化栈空间为 0 (安全) */
    uint8_t *stack_mem = (uint8_t *)virt;
    for (size_t i = 0; i < pages * PAGE_SIZE; i++) {
        stack_mem[i] = 0;
    }
    
    return virt + pages * PAGE_SIZE;  /* 栈顶 (高地址) */
}

/* 释放内核栈 (延迟释放) */
static void free_kernel_stack(uint64_t stack_top_addr) {
    if (stack_top_addr == 0) return;
    
    /* 尝试放入缓存 */
    if (kstack_cache_free_fast(stack_top_addr) == 0) {
        return;  /* 成功放入缓存 */
    }
    
    /* 缓存已满，添加到延迟释放列表 */
    while (__sync_lock_test_and_set(&kstack_global_lock, 1)) {
        asm volatile("pause");
    }
    
    if (kstack_delayed_count < KSTACK_DELAYED_FREE_MAX) {
        kstack_delayed_free[kstack_delayed_count++] = stack_top_addr;
    } else {
        /* 延迟释放列表也满了，强制释放最旧的栈 */
        uint64_t oldest = kstack_delayed_free[0];
        
        /* 移动队列（简单实现） */
        for (int i = 0; i < KSTACK_DELAYED_FREE_MAX - 1; i++) {
            kstack_delayed_free[i] = kstack_delayed_free[i + 1];
        }
        kstack_delayed_free[KSTACK_DELAYED_FREE_MAX - 1] = stack_top_addr;
        
        /* 释放最旧的栈 */
        uint64_t oldest_base = oldest - KERNEL_STACK_SIZE;
        size_t pages = KERNEL_STACK_SIZE / PAGE_SIZE;
        
        uint64_t phys = vmm_get_physical(vmm_get_kernel_space(), oldest_base);
        if (phys != 0) {
            vmm_unmap_range(vmm_get_kernel_space(), oldest_base, pages);
            pmm_free_pages(phys, pages);
        }
    }
    
    __sync_lock_release(&kstack_global_lock);
}

uint64_t get_tick_count(void) {
    return tick_count;
}

void increment_tick_count(void) {
    __sync_fetch_and_add(&tick_count, 1);
}

void task_init(void) {
    // println("  [DEBUG] task_init() starting...");
    
    /* 初始化 COW 子系统 */
    extern int cow_init(void);
    if (cow_init() != 0) {
        // println("  [ERROR] Failed to initialize COW subsystem!");
        /* COW 是必需的，失败则系统无法正常工作 */
        return;
    }
    // println("  [DEBUG] COW subsystem initialized");
    
    /* 初始化内核栈缓存池 */
    kstack_cache_init();
    // println("  [DEBUG] Kernel stack cache initialized");
    
    /* 测试 SMP 是否正常工作 */
    // print("  [DEBUG] Testing smp_processor_id()...");
    uint32_t cpu_id = smp_processor_id();
    // print(" returned ");
    // print_dec(cpu_id);
    // println("");
    
    // print("  [DEBUG] this_cpu() = 0x");
    struct cpu_local *cpu = this_cpu();
    // print_hex((uint64_t)cpu);
    // println("");
    
    if (cpu == NULL) {
        // println("  [ERROR] this_cpu() returned NULL in task_init()!");
        // println("  [ERROR] SMP initialization may have failed!");
        /* 尝试继续，但可能会崩溃 */
    } else {
        // print("  [DEBUG] CPU ");
        // print_dec(cpu->cpu_id);
        // print(" state: ");
        // print_dec(cpu->state);
        // println("");
    }
    
    /* 清空进程表 */
    // print("  [DEBUG] Clearing process table...");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i].state = PROCESS_STATE_UNUSED;
        process_table[i].pid = 0;
    }
    // println(" done");
    
    /* 清空线程表 */
    // print("  [DEBUG] Clearing thread table...");
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_table[i].state = THREAD_STATE_UNUSED;
        thread_table[i].tid = 0;
        /* 初始化所有链表头，防止 list_add_tail 损坏链表 */
        thread_table[i].run_list.next = &thread_table[i].run_list;
        thread_table[i].run_list.prev = &thread_table[i].run_list;
        thread_table[i].wait_list.next = &thread_table[i].wait_list;
        thread_table[i].wait_list.prev = &thread_table[i].wait_list;
        thread_table[i].cgroup_list.next = &thread_table[i].cgroup_list;
        thread_table[i].cgroup_list.prev = &thread_table[i].cgroup_list;
        thread_table[i].sleep_list.next = &thread_table[i].sleep_list;
        thread_table[i].sleep_list.prev = &thread_table[i].sleep_list;
    }
    // println(" done");
    
    /* 创建内核进程 */
    // print("  [DEBUG] Creating kernel process...");
    kernel_process = process_create("kernel", 0);
    if (kernel_process == NULL) {
        // println("");
        // println("  [ERROR] Failed to create kernel process!");
        return;
    }
    // println(" done");

    // print("  [DEBUG] kernel_process addr: 0x");
    // print_hex((uint64_t)kernel_process);
    // println("");

    /* idle 线程在 scheduler_init() 中创建，这里不再重复创建 */
    current_thread = NULL;
    this_cpu()->current_thread = NULL;

    // print("  [DEBUG] current_thread set to NULL (will be set by scheduler_init)");
    // println("");
    
    // print("    Kernel PID: ");
    // print_dec(kernel_process->pid);
    // println("");
    // print("    Max Processes: ");
    // print_dec(MAX_PROCESSES);
    // println("");
    // print("    Max Threads: ");
    // print_dec(MAX_THREADS);
    // println("");
    // print("    KStack Cache: ");
    // print_dec(KSTACK_CACHE_SIZE);
    // println(" per CPU");
    
    // println("  [DEBUG] task_init() completed");
}

struct process *process_get_current(void) {
    struct thread *cur = this_cpu()->current_thread;
    if (cur == NULL) {
        return NULL;
    }
    return process_get_by_pid(cur->pid);
}

uint32_t process_get_current_uid(void) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return 0;
    }
    return proc->uid;
}

uint32_t process_get_current_gid(void) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return 0;
    }
    return proc->gid;
}

struct process *process_get_by_pid(uint32_t pid) {
    /* 使用哈希表进行 O(1) 查找 */
    return pid_hash_lookup(pid);
}

/* 查找指定父进程的僵尸子进程 */
struct process *process_find_zombie_child(uint32_t parent_pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROCESS_STATE_ZOMBIE &&
            process_table[i].parent_pid == parent_pid) {
            return &process_table[i];
        }
    }
    return NULL;
}

struct process *process_create(const char *name, uint64_t entry_point) {
    /* 查找空闲槽 */
    int slot = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROCESS_STATE_UNUSED) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        return NULL;  /* 进程表已满 */
    }
    
    struct process *proc = &process_table[slot];
    
    /* 分配进程 ID (使用原子操作保证 SMP 安全) */
    proc->pid = __sync_fetch_and_add(&next_pid, 1);
    proc->parent_pid = 0;
    proc->state = PROCESS_STATE_RUNNING;
    proc->entry_point = entry_point;
    proc->threads = NULL;
    proc->thread_count = 0;
    proc->exit_code = 0;

    /* 初始化用户身份 */
    if (kernel_process == NULL) {
        /* 内核进程：root */
        proc->uid = 0;
        proc->gid = 0;
        proc->username[0] = 'r'; proc->username[1] = 'o'; proc->username[2] = 'o'; proc->username[3] = 't'; proc->username[4] = 0;
        proc->cwd[0] = '/'; proc->cwd[1] = '\0';
    } else {
        /* 普通进程：继承父进程 */
        struct process *parent = process_get_current();
        if (parent != NULL) {
            proc->uid = parent->uid;
            proc->gid = parent->gid;
            for (int i = 0; i < 32; i++) { proc->username[i] = parent->username[i]; if (!parent->username[i]) break; }
            for (int i = 0; i < 256; i++) { proc->cwd[i] = parent->cwd[i]; if (!parent->cwd[i]) break; }
        } else {
            proc->uid = 0;
            proc->gid = 0;
            proc->username[0] = 'r'; proc->username[1] = 'o'; proc->username[2] = 'o'; proc->username[3] = 't'; proc->username[4] = 0;
            proc->cwd[0] = '/'; proc->cwd[1] = '\0';
        }
    }
    
    /* 复制名称 */
    if (name != NULL) {
        int i = 0;
        while (name[i] && i < 63) {
            proc->name[i] = name[i];
            i++;
        }
        proc->name[i] = '\0';
    } else {
        proc->name[0] = '\0';
    }
    
    /* 创建地址空间 (内核进程使用内核空间) */
    if (kernel_process == NULL || proc->pid != kernel_process->pid) {
        proc->space = vmm_create_address_space();
        if (proc->space == NULL) {
            proc->state = PROCESS_STATE_UNUSED;
            return NULL;
        }
    } else {
        proc->space = vmm_get_kernel_space();
    }
    
    /* 创建文件描述符表并初始化 stdin/stdout/stderr */
    proc->fd_table = fd_table_create();
    if (proc->fd_table == NULL) {
        proc->state = PROCESS_STATE_UNUSED;
        return NULL;
    }
    
    /* 插入 PID 哈希表 */
    pid_hash_insert(proc);

    /* 初始化进程级资源限制 (从系统默认值) */
    rlimit_init_process(proc);

    return proc;
}

void process_destroy(struct process *proc) {
    if (proc == NULL) {
        return;
    }
    
    /* 从 PID 哈希表移除 */
    pid_hash_remove(proc);
    
    /* 销毁所有线程 */
    struct thread *thread = proc->threads;
    while (thread != NULL) {
        struct thread *next = thread->process_next;
        thread_destroy(thread);
        thread = next;
    }
    
    /* 销毁地址空间 */
    if (proc->space != vmm_get_kernel_space()) {
        vmm_destroy_address_space(proc->space);
    }
    
    proc->state = PROCESS_STATE_UNUSED;
}

struct thread *thread_get_current(void) {
    return this_cpu()->current_thread;
}

struct thread *thread_get_by_tid(uint32_t tid) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state != THREAD_STATE_UNUSED && 
            thread_table[i].tid == tid) {
            return &thread_table[i];
        }
    }
    return NULL;
}

struct thread *thread_create(struct process *proc, const char *name, 
                             uint64_t entry_point, void *arg) {
    if (proc == NULL) {
        return NULL;
    }
    
    /* 查找空闲槽 */
    int slot = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state == THREAD_STATE_UNUSED) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        return NULL;  /* 线程表已满 */
    }
    
    struct thread *thread = &thread_table[slot];
    
    /* 调试: 打印分配信息 */
    {
        extern void serial_print(const char *str);
        serial_print("[THREAD] slot=");
        char buf[24];
        int pos = 0;
        /* slot 数字 */
        if (slot >= 100) buf[pos++] = '0' + (slot / 100);
        if (slot >= 10) buf[pos++] = '0' + ((slot / 10) % 10);
        buf[pos++] = '0' + (slot % 10);
        buf[pos] = '\0';
        serial_print(buf);
        serial_print(" addr=0x");
        /* 十六进制地址 */
        const char *hex = "0123456789ABCDEF";
        uint64_t addr = (uint64_t)thread;
        pos = 0;
        for (int i = 60; i >= 0; i -= 4) {
            buf[pos++] = hex[(addr >> i) & 0xF];
        }
        buf[pos] = '\0';
        serial_print(buf);
        serial_print(" name=");
        serial_print(name ? name : "NULL");
        serial_print("\n");
    }
    
    /* 分配线程 ID (使用原子操作保证 SMP 安全) */
    thread->tid = __sync_fetch_and_add(&next_tid, 1);
    thread->pid = proc->pid;
    
    /* 设置状态和优先级 */
    thread->state = THREAD_STATE_READY;
    thread->priority = PRIORITY_NORMAL;
    thread->time_slice = DEFAULT_TIME_SLICE;
    thread->default_slice = DEFAULT_TIME_SLICE;
    
    /* 初始化实时调度参数 */
    thread->sched_policy = SCHED_NORMAL;
    thread->rt_priority = 0;
    thread->rt_time_slice = 0;

    /* 初始化优先级继承 (PI) 字段 */
    thread->boosted_priority = 0;
    thread->pi_original_prio = 0;
    thread->pi_waiting_for = NULL;
    INIT_LIST_HEAD(&thread->pi_waiters);
    INIT_LIST_HEAD(&thread->pi_list);
    thread->pi_lock = 0;
    
    /* 分配内核栈 (使用缓存优化) */
    thread->kernel_stack = alloc_kernel_stack(KERNEL_STACK_SIZE);
    thread->stack_size = KERNEL_STACK_SIZE;
    
    if (thread->kernel_stack == 0) {
        thread->state = THREAD_STATE_UNUSED;
        return NULL;
    }
    
    /* 初始化上下文 - 使用 volatile 防止编译器优化 */
    volatile struct cpu_context *ctx = &thread->context;
    
    /* 清零所有通用寄存器，避免随机值 */
    ctx->r15 = 0;
    ctx->r14 = 0;
    ctx->r13 = 0;
    ctx->r12 = 0;
    ctx->r11 = 0;
    ctx->r10 = 0;
    ctx->r9 = 0;
    ctx->r8 = 0;
    ctx->rdi = 0;
    ctx->rsi = 0;
    ctx->rbp = 0;
    ctx->rbx = 0;
    ctx->rdx = 0;
    ctx->rcx = 0;
    ctx->rax = 0;
    
    /* 设置关键寄存器 */
    ctx->rip = entry_point;
    ctx->cs = 0x08;  /* 内核代码段 */
    ctx->ss = 0x10;  /* 内核数据段 */
    ctx->rflags = 0x202;  /* IF = 1 */
    ctx->rsp = thread->kernel_stack;
    ctx->user_rsp = 0;  /* 用户线程会在 elf.c 中设置 */
    
    /* 强制内存屏障，确保写入完成 */
    __sync_synchronize();
    
    /* 调试输出 - 在屏障后读取 */
    {
        extern void serial_print(const char *str);
        serial_print("[THREAD_CREATE] tid=");
        char buf[24];
        int pos = 0;
        uint64_t tid = thread->tid;
        if (tid == 0) buf[pos++] = '0';
        else {
            uint64_t tmp = tid;
            int t = 0;
            char tmpbuf[24];
            while (tmp > 0) { tmpbuf[t++] = '0' + (tmp % 10); tmp /= 10; }
            while (t > 0) buf[pos++] = tmpbuf[--t];
        }
        buf[pos] = '\0';
        serial_print(buf);
        serial_print(" cs=0x");
        uint16_t cs = (uint16_t)ctx->cs;
        buf[0] = "0123456789ABCDEF"[(cs >> 12) & 0xF];
        buf[1] = "0123456789ABCDEF"[(cs >> 8) & 0xF];
        buf[2] = "0123456789ABCDEF"[(cs >> 4) & 0xF];
        buf[3] = "0123456789ABCDEF"[cs & 0xF];
        buf[4] = '\0';
        serial_print(buf);
        serial_print(" ss=0x");
        uint16_t ss = (uint16_t)ctx->ss;
        buf[0] = "0123456789ABCDEF"[(ss >> 12) & 0xF];
        buf[1] = "0123456789ABCDEF"[(ss >> 8) & 0xF];
        buf[2] = "0123456789ABCDEF"[(ss >> 4) & 0xF];
        buf[3] = "0123456789ABCDEF"[ss & 0xF];
        buf[4] = '\0';
        serial_print(buf);
        serial_print("\n");
    }
    
    /* 设置参数 (通过 RDI 传递第一个参数) */
    thread->context.rdi = (uint64_t)arg;
    
    /* 复制名称 */
    if (name != NULL) {
        int i = 0;
        while (name[i] && i < 31) {
            thread->name[i] = name[i];
            i++;
        }
        thread->name[i] = '\0';
    } else {
        thread->name[0] = '\0';
    }
    
    /* 添加到进程的线程链表 */
    thread->process_next = proc->threads;
    proc->threads = thread;
    proc->thread_count++;
    
    return thread;
}

/* 内核线程入口包装 */
struct kernel_thread_args {
    void (*entry)(void *);
    void *arg;
};

static void kernel_thread_wrapper(void *data) {
    struct kernel_thread_args *args = (struct kernel_thread_args *)data;
    void (*entry)(void *) = args->entry;
    void *arg = args->arg;
    
    /* 释放参数结构 */
    kfree(args);
    
    /* 调用实际入口 */
    entry(arg);
    
    /* 线程退出 */
    thread_exit(0);
}

struct thread *thread_create_kernel(const char *name, void (*entry)(void *), void *arg) {
    /* 使用内核进程 */
    struct process *proc = kernel_process;
    if (proc == NULL) {
        proc = process_get_current();
    }
    
    /* 创建参数结构 */
    struct kernel_thread_args *args = kmalloc(sizeof(struct kernel_thread_args));
    if (args == NULL) {
        return NULL;
    }
    args->entry = entry;
    args->arg = arg;
    
    /* 创建线程，入口为包装函数 */
    struct thread *thread = thread_create(proc, name, (uint64_t)kernel_thread_wrapper, args);
    if (thread == NULL) {
        kfree(args);
        return NULL;
    }
    
    return thread;
}

void thread_destroy(struct thread *thread) {
    if (thread == NULL) {
        return;
    }
    
    /* 释放内核栈 (如果已分配) */
    if (thread->kernel_stack != 0) {
        /* 检查是否为当前线程 */
        if (thread == this_cpu()->current_thread) {
            /* 当前线程不能直接释放栈，因为代码正在执行中
             * 使用延迟释放机制 */
            free_kernel_stack(thread->kernel_stack);
        } else {
            /* 非当前线程可以安全释放 */
            free_kernel_stack(thread->kernel_stack);
        }
    }
    
    /* 释放用户栈 (如果存在) */
    if (thread->user_stack != 0) {
        /* 用户栈在进程地址空间中，由进程销毁时统一释放 */
        thread->user_stack = 0;
    }
    
    /* 清理线程数据 */
    thread->state = THREAD_STATE_UNUSED;
    thread->kernel_stack = 0;
    thread->stack_size = 0;
    thread->cpu = -1;
    thread->flags = 0;
    thread->tid = 0;
}

void thread_set_state(struct thread *thread, thread_state_t state) {
    if (thread == NULL) {
        return;
    }
    thread->state = state;
}

void thread_sleep(uint64_t ticks) {
    struct thread *cur = this_cpu()->current_thread;
    if (cur == NULL) {
        return;
    }

    cur->state = THREAD_STATE_SLEEPING;
    cur->wake_time = get_tick_count() + ticks;

    /* 诊断输出: 记录线程睡眠 (限制前 20 次输出避免日志过多) */
    {
        static int _sleep_dbg_cnt = 0;
        if (_sleep_dbg_cnt < 20) {
            _sleep_dbg_cnt++;
            int _sleep_cpu = smp_processor_id();
            serial_print("[SLEEP] tid=");
            { char _nb[16]; int _np=0; long _v=(long)cur->tid; if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
            serial_print(" cpu=");
            { char _nb[8]; int _np=0; int _v=_sleep_cpu; if(_v==0){_nb[_np++]='0';}else{char _tb[8];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
            serial_print(" wake=");
            { char _nb[16]; int _np=0; unsigned long _v=(unsigned long)cur->wake_time; if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
            serial_print(" ticks=");
            { char _nb[16]; int _np=0; unsigned long _v=(unsigned long)ticks; if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
            serial_print("\n");
        }
    }

    int cpu = smp_processor_id();
    struct sleep_queue *sq = &percpu_sleepq[cpu];
    while (__sync_lock_test_and_set(&sq->lock, 1)) {
        asm volatile("pause");
    }
    list_add_tail(&cur->sleep_list, &sq->list);
    __sync_lock_release(&sq->lock);

    /* 设置需要重新调度标志，在下一次调度点切换线程 */
    cur->flags |= THREAD_FLAG_NEED_RESCHED;
    extern void schedule(void);
    schedule();
}

void thread_exit(int code) {
    (void)code;
    
    struct thread *cur = this_cpu()->current_thread;
    if (cur == NULL) {
        return;
    }
    
    cur->state = THREAD_STATE_TERMINATED;
    cur->flags |= THREAD_FLAG_NEED_RESCHED;
    
    /* 直接调用 schedule() 切换到其他线程
     * 不再使用 int $48 (APIC 定时器中断向量)，因为：
     * 1. int $48 会走完整 ISR 流程，可能不会触发调度
     * 2. 已终止线程可能从 ISR 返回继续执行
     * 3. 与 process_exit() 保持一致 */
    extern void schedule(void);
    schedule();
}

void process_exit(int code) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return;
    }

    serial_print("[PROCESS_EXIT] pid=");
    { char _nb[16]; int _np=0; long _v=(long)proc->pid; if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
    serial_print(" code=");
    { char _nb[16]; int _np=0; long _v=(long)code; if(_v<0){serial_print("-");_v=-_v;} if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
    serial_print("\n");

    proc->exit_code = code;
    proc->state = PROCESS_STATE_ZOMBIE;

    /* 终止所有线程 */
    struct thread *thread = proc->threads;
    while (thread != NULL) {
        thread->state = THREAD_STATE_TERMINATED;

        /* 从睡眠队列中移除（如果线程正在睡眠） */
        if (!list_empty(&thread->sleep_list)) {
            list_del_init(&thread->sleep_list);
        }

        thread = thread->process_next;
    }

    /* 唤醒父进程（如果它在等待子进程） */
    extern void wakeup_parent(pid_t child_pid, int status);
    wakeup_parent(proc->pid, code);

    /* 直接调用 schedule() 而不是触发中断
     * int $48 是 APIC 定时器中断，不应该在这里使用
     * schedule() 会切换到另一个进程，不会返回到当前进程
     * 因为当前线程状态是 TERMINATED，pick_next_thread 会跳过它
     */
    extern void schedule(void);
    schedule();

    /* 如果 schedule() 返回（不应该发生），停机 */
    while (1) {
        asm volatile("hlt");
    }
}

/* 复制地址空间 (用于 fork)
 * 注意：如果失败，调用者需要调用 vmm_destroy_address_space 来清理
 */
static int copy_address_space(struct address_space *dst, struct address_space *src) {
    if (dst == NULL || src == NULL) {
        return -1;
    }
    
    pte_t *src_pml4 = src->pml4;
    pte_t *dst_pml4 = dst->pml4;
    
    /* 遍历用户空间 (0-256) */
    for (int i = 0; i < 256; i++) {
        if (src_pml4[i] & VMM_FLAG_PRESENT) {
            pte_t *src_pdpt = (pte_t *)phys_to_virt(pte_get_addr(src_pml4[i]));
            
            /* 分配新的 PDPT */
            uint64_t pdpt_phys = pmm_alloc_page();
            if (pdpt_phys == 0) goto rollback;
            
            pte_t *dst_pdpt = (pte_t *)phys_to_virt(pdpt_phys);
            for (int j = 0; j < 512; j++) dst_pdpt[j] = 0;
            
            dst_pml4[i] = pte_create(pdpt_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER);
            
            /* 遍历 PDPT */
            for (int j = 0; j < 512; j++) {
                if (src_pdpt[j] & VMM_FLAG_PRESENT) {
                    pte_t *src_pd = (pte_t *)phys_to_virt(pte_get_addr(src_pdpt[j]));
                    
                    uint64_t pd_phys = pmm_alloc_page();
                    if (pd_phys == 0) goto rollback;
                    
                    pte_t *dst_pd = (pte_t *)phys_to_virt(pd_phys);
                    for (int k = 0; k < 512; k++) dst_pd[k] = 0;
                    
                    dst_pdpt[j] = pte_create(pd_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER);
                    
                    /* 遍历 PD */
                    for (int k = 0; k < 512; k++) {
                        if (src_pd[k] & VMM_FLAG_PRESENT) {
                            pte_t *src_pt = (pte_t *)phys_to_virt(pte_get_addr(src_pd[k]));
                            
                            uint64_t pt_phys = pmm_alloc_page();
                            if (pt_phys == 0) goto rollback;
                            
                            pte_t *dst_pt = (pte_t *)phys_to_virt(pt_phys);
                            for (int l = 0; l < 512; l++) dst_pt[l] = 0;
                            
                            dst_pd[k] = pte_create(pt_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER);
                            
                            /* 遍历 PT - 实现真正的写时复制 */
                            for (int l = 0; l < 512; l++) {
                                if (src_pt[l] & VMM_FLAG_PRESENT) {
                                    uint64_t src_phys = pte_get_addr(src_pt[l]);
                                    uint64_t flags = pte_get_flags(src_pt[l]);
                                    
                                    /* 增加源页面的引用计数 */
                                    if (cow_inc_page_ref(src_phys) != 0) {
                                        /* 如果 COW 不支持，回退到复制 */
                                        uint64_t new_phys = pmm_alloc_page();
                                        if (new_phys == 0) goto rollback;
                                        
                                        void *src_page = phys_to_virt(src_phys);
                                        void *dst_page = phys_to_virt(new_phys);
                                        
                                        if (src_page != NULL && dst_page != NULL) {
                                            /* 复制页面内容 */
                                            for (size_t m = 0; m < PAGE_SIZE; m++) {
                                                ((uint8_t *)dst_page)[m] = ((uint8_t *)src_page)[m];
                                            }
                                        }
                                        
                                        dst_pt[l] = pte_create(new_phys, flags);
                                    } else {
                                        /* 设置为只读，启用 COW */
                                        uint64_t cow_flags = flags & ~VMM_FLAG_WRITABLE;
                                        cow_flags |= VMM_FLAG_COW;  /* 使用 COW 标志 */
                                        dst_pt[l] = pte_create(src_phys, cow_flags);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    return 0;
    
rollback:
    /* 销毁部分创建的地址空间 */
    vmm_destroy_address_space(dst);
    return -1;
}

struct process *process_fork(struct process *parent) {
    if (parent == NULL) {
        return NULL;
    }
    
    /* 创建新进程 */
    struct process *child = process_create(parent->name, parent->entry_point);
    if (child == NULL) {
        return NULL;
    }
    
    /* 设置父进程 */
    child->parent_pid = parent->pid;
    
    /* 复制 ABI 类型 */
    child->abi = parent->abi;

    /* 复制用户身份 */
    child->uid = parent->uid;
    child->gid = parent->gid;

    /* 继承父进程的资源限制 */
    rlimit_inherit_process(child, parent);
    
    /* 销毁默认创建的地址空间，复制父进程的 */
    if (child->space != NULL && child->space != vmm_get_kernel_space()) {
        vmm_destroy_address_space(child->space);
    }
    
    child->space = vmm_create_address_space();
    if (child->space == NULL) {
        child->state = PROCESS_STATE_UNUSED;
        return NULL;
    }
    
    /* 复制地址空间 */
    if (copy_address_space(child->space, parent->space) != 0) {
        vmm_destroy_address_space(child->space);
        child->state = PROCESS_STATE_UNUSED;
        return NULL;
    }
    
    /* 为子进程创建主线程 */
    struct thread *parent_thread = parent->threads;
    if (parent_thread != NULL) {
        struct thread *child_thread = thread_create(child, parent_thread->name,
                                                     parent_thread->context.rip, NULL);
        if (child_thread == NULL) {
            process_destroy(child);
            return NULL;
        }
        
        /* 复制寄存器上下文 */
        child_thread->context = parent_thread->context;
        
        /* 复制栈信息 */
        child_thread->user_stack = parent_thread->user_stack;
        child_thread->stack_size = parent_thread->stack_size;
        
        /* fork 返回值：子进程返回 0 */
        child_thread->context.rax = 0;
    }
    
    return child;
}

uint32_t get_thread_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state != THREAD_STATE_UNUSED) {
            count++;
        }
    }
    return count;
}

uint32_t get_process_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state != PROCESS_STATE_UNUSED) {
            count++;
        }
    }
    return count;
}

void task_stats(void) {
    // println("[TASK] ===== Task Statistics =====");
    
    // print("  Processes: ");
    // print_dec(get_process_count());
    // print(" / ");
    // print_dec(MAX_PROCESSES);
    // println("");
    
    // print("  Threads:   ");
    // print_dec(get_thread_count());
    // print(" / ");
    // print_dec(MAX_THREADS);
    // println("");
    
    // print("  Next PID:  ");
    // print_dec(next_pid);
    // println("");
    
    // print("  Next TID:  ");
    // print_dec(next_tid);
    // println("");
    
    // print("  Tick:      ");
    // print_dec(tick_count);
    // println("");
}

/* ========== 内核栈保护功能 ========== */

/* 栈金丝雀魔数 */
#define STACK_CANARY_MAGIC  0xDEADC0DEBEEFCAFEULL

/* 栈保护统计 */
struct stack_prot_stats {
    uint64_t canary_checks;
    uint64_t canary_failures;
    uint64_t overflow_detected;
    uint64_t underflow_detected;
};

static struct stack_prot_stats stack_stats;

/* 在栈底设置金丝雀 */
void stack_set_canary(uint64_t stack_top) {
    /* 栈顶向下第一个位置存放金丝雀 */
    uint64_t *canary = (uint64_t *)(stack_top - 8);
    *canary = STACK_CANARY_MAGIC;
}

/* 验证栈金丝雀 */
int stack_verify_canary(uint64_t stack_top) {
    uint64_t *canary = (uint64_t *)(stack_top - 8);
    
    stack_stats.canary_checks++;
    
    if (*canary != STACK_CANARY_MAGIC) {
        stack_stats.canary_failures++;
        // println("[STACK] Stack canary corrupted! Possible overflow detected.");
        // print("  Expected: ");
        // print_hex(STACK_CANARY_MAGIC);
        // println("");
        // print("  Found:    ");
        // print_hex(*canary);
        // println("");
        return -1;
    }
    
    return 0;
}

/* 栈溢出检测 */
int stack_check_overflow(uint64_t stack_top, uint64_t stack_bottom, uint64_t sp) {
    if (sp < stack_bottom || sp > stack_top) {
        stack_stats.overflow_detected++;
        // println("[STACK] Stack pointer out of bounds!");
        // print("  Stack top:    ");
        // print_hex(stack_top);
        // println("");
        // print("  Stack bottom: ");
        // print_hex(stack_bottom);
        // println("");
        // print("  SP:           ");
        // print_hex(sp);
        // println("");
        return -1;
    }
    return 0;
}

/* 获取栈使用量 */
size_t stack_get_usage(uint64_t stack_top, uint64_t sp) {
    if (sp > stack_top) return 0;
    return stack_top - sp;
}

/* 检查栈是否接近溢出 */
int stack_near_overflow(uint64_t stack_top, uint64_t sp, size_t threshold) {
    size_t usage = stack_get_usage(stack_top, sp);
    size_t stack_size = 8192;  /* 默认栈大小 */
    
    if (usage > stack_size - threshold) {
        return 1;  /* 接近溢出 */
    }
    return 0;
}

/* 栈统计初始化 */
void stack_stats_init(void) {
    stack_stats.canary_checks = 0;
    stack_stats.canary_failures = 0;
    stack_stats.overflow_detected = 0;
    stack_stats.underflow_detected = 0;
}

/* 获取栈保护统计 */
void stack_get_stats(struct stack_prot_stats *stats) {
    if (stats != NULL) {
        *stats = stack_stats;
    }
}

/* ========== 线程安全检查 ========== */

/* 检查线程是否有效并安全 */
int thread_validate(struct thread *thread) {
    if (thread == NULL) return 0;
    
    /* 检查 TID 范围 */
    if (thread->tid == 0 || thread->tid >= MAX_THREADS) return 0;
    
    /* 检查状态有效性 */
    if (thread->state > THREAD_STATE_TERMINATED) return 0;
    
    /* 检查栈指针 */
    if (thread->kernel_stack == 0) return 0;
    
    return 1;
}

/* 检查进程是否有效并安全 */
int process_validate(struct process *proc) {
    if (proc == NULL) return 0;
    
    /* 检查 PID 范围 */
    if (proc->pid == 0 || proc->pid >= MAX_PROCESSES) return 0;
    
    /* 检查状态有效性 */
    if (proc->state > PROCESS_STATE_ZOMBIE) return 0;
    
    return 1;
}

/* 安全获取当前进程 */
struct process *safe_get_current_process(void) {
    struct process *proc = process_get_current();
    
    if (!process_validate(proc)) {
        return NULL;
    }
    
    return proc;
}

/* 安全获取当前线程 */
struct thread *safe_get_current_thread(void) {
    struct thread *thread = thread_get_current();
    
    if (!thread_validate(thread)) {
        return NULL;
    }
    
    return thread;
}

/* ========== 任务完整性检查 ========== */

/* 检查所有任务完整性 */
int task_check_integrity(void) {
    int errors = 0;
    
    /* 检查所有线程 */
    for (uint32_t i = 0; i < MAX_THREADS; i++) {
        struct thread *thread = &thread_table[i];
        
        if (thread->state != THREAD_STATE_UNUSED) {
            /* 检查栈金丝雀 */
            if (thread->kernel_stack != 0) {
                if (stack_verify_canary(thread->kernel_stack + thread->stack_size) != 0) {
                    // print("[TASK] Thread ");
                    // print_dec(thread->tid);
                    // println(" has corrupted stack");
                    errors++;
                }
            }
            
            /* 检查进程关联 */
            if (thread->pid >= MAX_PROCESSES) {
                // print("[TASK] Thread ");
                // print_dec(thread->tid);
                // println(" has invalid PID");
                errors++;
            }
        }
    }
    
    /* 检查所有进程 */
    for (uint32_t i = 0; i < MAX_PROCESSES; i++) {
        struct process *proc = &process_table[i];
        
        if (proc->state != PROCESS_STATE_UNUSED) {
            /* 检查线程链 */
            int found_threads = 0;
            struct thread *t = proc->threads;
            while (t != NULL && found_threads < 100) {  /* 防止死循环 */
                found_threads++;
                t = t->process_next;
            }
            
            if (found_threads != proc->thread_count) {
                // print("[TASK] Process ");
                // print_dec(proc->pid);
                // println(" has inconsistent thread count");
                errors++;
            }
        }
    }
    
    return errors == 0 ? 0 : -1;
}

/* ========== 任务诊断 ========== */

/* 打印线程详细信息 */
void thread_dump(struct thread *thread) {
    if (thread == NULL) {
        // println("NULL thread");
        return;
    }
    
    // println("=== Thread Dump ===");
    // print("TID:          ");
    // print_dec(thread->tid);
    // println("");
    // print("PID:          ");
    // print_dec(thread->pid);
    // println("");
    // print("State:        ");
    // print_dec(thread->state);
    // println("");
    // print("Priority:     ");
    // print_dec(thread->priority);
    // println("");
    // print("CPU:          ");
    // print_dec(thread->cpu);
    // println("");
    // print("Kernel Stack: ");
    // print_hex(thread->kernel_stack);
    // println("");
    // print("User Stack:   ");
    // print_hex(thread->user_stack);
    // println("");
    // print("Stack Size:   ");
    // print_dec(thread->stack_size);
    // println("");
    // print("Name:         ");
    // println(thread->name);
}

/* 打印进程详细信息 */
void process_dump(struct process *proc) {
    if (proc == NULL) {
        // println("NULL process");
        return;
    }
    
    // println("=== Process Dump ===");
    // print("PID:         ");
    // print_dec(proc->pid);
    // println("");
    // print("Parent PID:  ");
    // print_dec(proc->parent_pid);
    // println("");
    // print("State:       ");
    // print_dec(proc->state);
    // println("");
    // print("Thread Cnt:  ");
    // print_dec(proc->thread_count);
    // println("");
    // print("Entry Point: ");
    // print_hex(proc->entry_point);
    // println("");
    // print("ABI:         ");
    // print_dec(proc->abi);
    // println("");
    // print("Exit Code:   ");
    // print_dec(proc->exit_code);
    // println("");
    // print("Name:        ");
    // println(proc->name);
}

/* ========== 进程管理高级增强功能 ========== */

/* === 进程组管理 === */

struct process_group {
    uint32_t pgid;              /* 进程组 ID */
    uint32_t leader;            /* 组长进程 PID */
    uint32_t members[MAX_PROCESSES];
    uint32_t member_count;
    int valid;
};

#define PG_MAX_GROUPS    64
static struct process_group pg_groups[PG_MAX_GROUPS];
static int pg_count = 0;

/* 创建进程组 */
int process_create_group(uint32_t pgid, uint32_t leader_pid) {
    if (pg_count >= PG_MAX_GROUPS) return -1;
    
    for (int i = 0; i < PG_MAX_GROUPS; i++) {
        if (!pg_groups[i].valid) {
            pg_groups[i].pgid = pgid;
            pg_groups[i].leader = leader_pid;
            pg_groups[i].members[0] = leader_pid;
            pg_groups[i].member_count = 1;
            pg_groups[i].valid = 1;
            pg_count++;
            return i;
        }
    }
    return -1;
}

/* 加入进程组 */
int process_join_group(uint32_t pgid, uint32_t pid) {
    for (int i = 0; i < PG_MAX_GROUPS; i++) {
        if (pg_groups[i].valid && pg_groups[i].pgid == pgid) {
            if (pg_groups[i].member_count < MAX_PROCESSES) {
                pg_groups[i].members[pg_groups[i].member_count++] = pid;
                return 0;
            }
        }
    }
    return -1;
}

/* 离开进程组 */
int process_leave_group(uint32_t pgid, uint32_t pid) {
    for (int i = 0; i < PG_MAX_GROUPS; i++) {
        if (pg_groups[i].valid && pg_groups[i].pgid == pgid) {
            for (uint32_t j = 0; j < pg_groups[i].member_count; j++) {
                if (pg_groups[i].members[j] == pid) {
                    for (uint32_t k = j; k < pg_groups[i].member_count - 1; k++) {
                        pg_groups[i].members[k] = pg_groups[i].members[k + 1];
                    }
                    pg_groups[i].member_count--;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* === 会话管理 === */

struct session {
    uint32_t sid;               /* 会话 ID */
    uint32_t leader;            /* 会话首进程 */
    uint32_t controlling_tty;   /* 控制终端 */
    int valid;
};

#define SESSION_MAX    32
static struct session sessions[SESSION_MAX];
static int session_count = 0;

/* 创建会话 */
int session_create(uint32_t sid, uint32_t leader_pid) {
    if (session_count >= SESSION_MAX) return -1;
    
    for (int i = 0; i < SESSION_MAX; i++) {
        if (!sessions[i].valid) {
            sessions[i].sid = sid;
            sessions[i].leader = leader_pid;
            sessions[i].controlling_tty = 0;
            sessions[i].valid = 1;
            session_count++;
            return i;
        }
    }
    return -1;
}

/* === 进程资源限制 === */

struct rlimit {
    uint64_t rlim_cur;          /* 软限制 */
    uint64_t rlim_max;          /* 硬限制 */
};

#define RLIMIT_CPU       0      /* CPU 时间 (秒) */
#define RLIMIT_DATA      1      /* 数据段大小 */
#define RLIMIT_STACK     2      /* 栈大小 */
#define RLIMIT_AS        3      /* 地址空间大小 */
#define RLIMIT_NOFILE    4      /* 打开文件数 */
#define RLIMIT_NPROC     5      /* 进程数 */
#define RLIMIT_MEMLOCK   6      /* 锁定内存大小 */
#define RLIMIT_COUNT     7

static struct rlimit process_rlimits[MAX_PROCESSES][RLIMIT_COUNT];

/* 设置资源限制 */
int process_set_rlimit(uint32_t pid, int resource, uint64_t cur, uint64_t max) {
    if (pid >= MAX_PROCESSES || resource < 0 || resource >= RLIMIT_COUNT) return -1;
    
    process_rlimits[pid][resource].rlim_cur = cur;
    process_rlimits[pid][resource].rlim_max = max;
    return 0;
}

/* 获取资源限制 */
int process_get_rlimit(uint32_t pid, int resource, uint64_t *cur, uint64_t *max) {
    if (pid >= MAX_PROCESSES || resource < 0 || resource >= RLIMIT_COUNT) return -1;
    
    *cur = process_rlimits[pid][resource].rlim_cur;
    *max = process_rlimits[pid][resource].rlim_max;
    return 0;
}

/* 检查资源限制 */
int process_check_limit(uint32_t pid, int resource, uint64_t value) {
    if (pid >= MAX_PROCESSES || resource < 0 || resource >= RLIMIT_COUNT) return 0;
    
    if (value > process_rlimits[pid][resource].rlim_cur) {
        return -1;  /* 超过软限制 */
    }
    return 0;
}

/* === 进程统计 === */

struct process_stats {
    uint64_t utime;             /* 用户态 CPU 时间 */
    uint64_t stime;             /* 内核态 CPU 时间 */
    uint64_t cutime;            /* 子进程用户态时间 */
    uint64_t cstime;            /* 子进程内核态时间 */
    uint64_t start_time;        /* 启动时间 */
    uint64_t vsize;             /* 虚拟内存大小 */
    uint64_t rss;               /* 驻留内存大小 */
    uint64_t minor_faults;
    uint64_t major_faults;
    uint32_t num_threads;
    uint32_t num_fds;
};

static struct process_stats proc_stats[MAX_PROCESSES];

/* 更新进程统计 */
void process_update_stats(uint32_t pid, int stat_type, uint64_t value) {
    if (pid >= MAX_PROCESSES) return;
    
    switch (stat_type) {
        case 0: proc_stats[pid].utime += value; break;
        case 1: proc_stats[pid].stime += value; break;
        case 2: proc_stats[pid].minor_faults += value; break;
        case 3: proc_stats[pid].major_faults += value; break;
        case 4: proc_stats[pid].vsize = value; break;
        case 5: proc_stats[pid].rss = value; break;
    }
}

/* 获取进程统计 */
void process_get_stats(uint32_t pid, struct process_stats *stats) {
    if (pid < MAX_PROCESSES && stats != NULL) {
        *stats = proc_stats[pid];
    }
}

/* === 进程优先级管理 === */

#define PRIO_MIN    -20
#define PRIO_MAX    20
#define PRIO_DEFAULT 0

/* 设置进程优先级 */
int process_set_priority(uint32_t pid, int priority) {
    if (pid >= MAX_PROCESSES) return -1;
    if (priority < PRIO_MIN) priority = PRIO_MIN;
    if (priority > PRIO_MAX) priority = PRIO_MAX;
    
    struct process *proc = &process_table[pid];
    if (proc->state == PROCESS_STATE_UNUSED) return -1;
    
    // proc->priority = priority + 100;  /* 转换为内部优先级 */
    return 0;
}

/* 获取进程优先级 */
int process_get_priority(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return -1;
    
    struct process *proc = &process_table[pid];
    if (proc->state == PROCESS_STATE_UNUSED) return -1;
    
    return 0; /* proc->priority - 100 */;
}

/* === 进程亲和性 === */

/* 设置 CPU 亲和性 */
int process_set_affinity(uint32_t pid, uint64_t cpu_mask) {
    if (pid >= MAX_PROCESSES) return -1;
    
    struct process *proc = &process_table[pid];
    if (proc->state == PROCESS_STATE_UNUSED) return -1;
    
    // proc->cpu_affinity = cpu_mask;
    return 0;
}

/* 获取 CPU 亲和性 */
uint64_t process_get_affinity(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return 0;
    
    struct process *proc = &process_table[pid];
    return 0; /* proc->cpu_affinity */;
}

/* === 进程诊断 === */

/* 打印所有进程列表 */
void process_print_all(void) {
    // println("=== Process List ===");
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state != PROCESS_STATE_UNUSED) {
            // print("PID ");
            // print_dec(process_table[i].pid);
            // print(" State ");
            // print_dec(process_table[i].state);
            // print(" Threads ");
            // print_dec(process_table[i].thread_count);
            // print(" ");
            // println(process_table[i].name);
        }
    }
}

/* 检查进程状态 */
int process_health_check(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return -1;
    
    struct process *proc = &process_table[pid];
    if (proc->state == PROCESS_STATE_UNUSED) return -1;
    
    /* 检查僵尸进程 */
    if (proc->state == PROCESS_STATE_ZOMBIE) {
        return 1;  /* 需要清理 */
    }
    
    /* 检查进程是否响应 */
    return 0;
}

/* 查找进程 by name */
int process_find_by_name(const char *name) {
    if (name == NULL) return -1;
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state != PROCESS_STATE_UNUSED) {
            if (strncmp(process_table[i].name, name, 31) == 0) {
                return i;
            }
        }
    }
    return -1;
}

/* === 进程树管理 === */

/* 获取子进程列表 */
int process_get_children(uint32_t pid, uint32_t *children, uint32_t *count) {
    if (pid >= MAX_PROCESSES || children == NULL || count == NULL) return -1;
    
    uint32_t found = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state != PROCESS_STATE_UNUSED &&
            process_table[i].parent_pid == pid) {
            children[found++] = process_table[i].pid;
        }
    }
    
    *count = found;
    return 0;
}

/* 获取进程树深度 */
int process_get_depth(uint32_t pid) {
    int depth = 0;
    uint32_t current = pid;
    
    while (current > 0 && depth < 100) {
        if (current >= MAX_PROCESSES) break;
        current = process_table[current].parent_pid;
        depth++;
    }
    
    return depth;
}

/* ========== 进程高级增强功能 ========== */

/* === 进程资源统计增强 === */

struct process_resource_usage {
    uint64_t cpu_time_total;      /* 总 CPU 时间 (纳秒) */
    uint64_t cpu_time_user;       /* 用户态 CPU 时间 */
    uint64_t cpu_time_kernel;     /* 内核态 CPU 时间 */
    uint64_t voluntary_switches;  /* 自愿上下文切换 */
    uint64_t involuntary_switches; /* 非自愿上下文切换 */
    uint64_t page_faults_minor;   /* 轻微缺页 */
    uint64_t page_faults_major;   /* 严重缺页 */
    uint64_t block_read_bytes;    /* 块设备读取字节 */
    uint64_t block_write_bytes;   /* 块设备写入字节 */
    uint64_t block_read_count;    /* 块设备读取次数 */
    uint64_t block_write_count;   /* 块设备写入次数 */
    uint64_t ipc_messages_sent;   /* IPC 消息发送 */
    uint64_t ipc_messages_recv;   /* IPC 消息接收 */
    uint64_t signals_delivered;   /* 信号投递数 */
    uint64_t file_reads;          /* 文件读取次数 */
    uint64_t file_writes;         /* 文件写入次数 */
    uint64_t network_bytes_recv;  /* 网络接收字节 */
    uint64_t network_bytes_sent;  /* 网络发送字节 */
    uint64_t memory_peak;         /* 峰值内存使用 */
    uint64_t threads_peak;        /* 峰值线程数 */
    uint64_t io_wait_time;        /* I/O 等待时间 */
};

static struct process_resource_usage resource_usage[MAX_PROCESSES];

/* 更新资源使用 */
void process_update_usage(uint32_t pid, int type, uint64_t value) {
    if (pid >= MAX_PROCESSES) return;
    
    switch (type) {
        case 0: resource_usage[pid].cpu_time_total += value; break;
        case 1: resource_usage[pid].cpu_time_user += value; break;
        case 2: resource_usage[pid].cpu_time_kernel += value; break;
        case 3: resource_usage[pid].voluntary_switches += value; break;
        case 4: resource_usage[pid].involuntary_switches += value; break;
        case 5: resource_usage[pid].page_faults_minor += value; break;
        case 6: resource_usage[pid].page_faults_major += value; break;
        case 7: resource_usage[pid].block_read_bytes += value; break;
        case 8: resource_usage[pid].block_write_bytes += value; break;
        case 9: resource_usage[pid].network_bytes_recv += value; break;
        case 10: resource_usage[pid].network_bytes_sent += value; break;
    }
}

/* 获取资源使用 */
void process_get_usage(uint32_t pid, struct process_resource_usage *usage) {
    if (pid < MAX_PROCESSES && usage != NULL) {
        *usage = resource_usage[pid];
    }
}

/* 重置资源统计 */
void process_reset_usage(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return;
    
    for (size_t i = 0; i < sizeof(resource_usage[pid]); i++) {
        ((uint8_t *)&resource_usage[pid])[i] = 0;
    }
}

/* === 进程检查点/恢复 === */

#define CHECKPOINT_MAX_SIZE    (16 * 1024 * 1024)  /* 16 MB */

struct process_checkpoint {
    uint32_t pid;
    uint64_t timestamp;
    char name[64];
    
    /* 内存状态 */
    struct {
        uint64_t start;
        uint64_t end;
        uint8_t *data;
    } memory_regions[16];
    int region_count;
    
    /* 寄存器状态 */
    uint64_t regs[23];  /* RAX-R15, RIP, RSP, RBP, RFLAGS */
    
    /* 文件描述符状态 */
    struct {
        int fd;
        char path[256];
        uint64_t offset;
        int flags;
    } files[32];
    int file_count;
    
    int valid;
};

static struct process_checkpoint checkpoints[MAX_PROCESSES];
static volatile int checkpoint_lock = 0;

/* 创建检查点 */
int process_create_checkpoint(uint32_t pid, const char *name) {
    if (pid >= MAX_PROCESSES) return -1;
    
    struct process *proc = &process_table[pid];
    if (proc->state == PROCESS_STATE_UNUSED) return -1;
    
    while (__sync_lock_test_and_set(&checkpoint_lock, 1)) {
        asm volatile("pause");
    }
    
    struct process_checkpoint *cp = &checkpoints[pid];
    
    cp->pid = pid;
    cp->timestamp = get_tick_count();
    
    if (name) {
        int i = 0;
        while (name[i] && i < 63) {
            cp->name[i] = name[i];
            i++;
        }
        cp->name[i] = '\0';
    } else {
        cp->name[0] = '\0';
    }
    
    /* 保存内存区域 (简化：只保存地址空间信息) */
    cp->region_count = 0;
    /* 实际实现需要遍历进程的 VMA */
    
    /* 保存文件描述符状态 (简化) */
    cp->file_count = 0;
    
    cp->valid = 1;
    
    __sync_lock_release(&checkpoint_lock);
    
    // print("[TASK] Checkpoint created for PID ");
    // print_dec(pid);
    // println("");
    
    return 0;
}

/* 从检查点恢复 */
int process_restore_checkpoint(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return -1;
    
    while (__sync_lock_test_and_set(&checkpoint_lock, 1)) {
        asm volatile("pause");
    }
    
    struct process_checkpoint *cp = &checkpoints[pid];
    
    if (!cp->valid) {
        __sync_lock_release(&checkpoint_lock);
        return -1;
    }
    
    /* 恢复内存区域 */
    for (int i = 0; i < cp->region_count; i++) {
        /* 实际实现需要恢复内存内容 */
    }
    
    /* 恢复文件描述符 */
    for (int i = 0; i < cp->file_count; i++) {
        /* 实际实现需要重新打开文件 */
    }
    
    __sync_lock_release(&checkpoint_lock);
    
    // print("[TASK] Checkpoint restored for PID ");
    // print_dec(pid);
    // println("");
    
    return 0;
}

/* 删除检查点 */
void process_delete_checkpoint(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return;
    
    while (__sync_lock_test_and_set(&checkpoint_lock, 1)) {
        asm volatile("pause");
    }
    
    checkpoints[pid].valid = 0;
    
    /* 释放内存 */
    for (int i = 0; i < checkpoints[pid].region_count; i++) {
        if (checkpoints[pid].memory_regions[i].data) {
            kfree(checkpoints[pid].memory_regions[i].data);
            checkpoints[pid].memory_regions[i].data = NULL;
        }
    }
    
    __sync_lock_release(&checkpoint_lock);
}

/* === 进程命名空间支持 === */

#define NS_MAX_TYPES    6
#define NS_MAX_DEPTH    32

enum ns_type {
    NS_MNT = 0,    /* 挂载命名空间 */
    NS_UTS,        /* UTS 命名空间 */
    NS_IPC,        /* IPC 命名空间 */
    NS_NET,        /* 网络命名空间 */
    NS_PID,        /* PID 命名空间 */
    NS_USER        /* 用户命名空间 */
};

struct process_namespace {
    uint32_t ns_id[NS_MAX_TYPES];
    uint32_t parent_ns[NS_MAX_TYPES];
    uint64_t creation_time;
    int active;
};

static struct process_namespace proc_ns[MAX_PROCESSES];
static volatile int ns_lock = 0;

/* 初始化命名空间 */
void process_ns_init(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return;
    
    for (int i = 0; i < NS_MAX_TYPES; i++) {
        proc_ns[pid].ns_id[i] = 0;  /* 默认根命名空间 */
        proc_ns[pid].parent_ns[i] = 0;
    }
    proc_ns[pid].active = 1;
}

/* 创建新命名空间 */
int process_create_ns(uint32_t pid, enum ns_type type) {
    if (pid >= MAX_PROCESSES || type >= NS_MAX_TYPES) return -1;
    
    while (__sync_lock_test_and_set(&ns_lock, 1)) {
        asm volatile("pause");
    }
    
    static uint32_t next_ns_id = 1;
    
    proc_ns[pid].parent_ns[type] = proc_ns[pid].ns_id[type];
    proc_ns[pid].ns_id[type] = next_ns_id++;
    
    __sync_lock_release(&ns_lock);
    
    return 0;
}

/* 加入命名空间 */
int process_join_ns(uint32_t pid, uint32_t target_pid, enum ns_type type) {
    if (pid >= MAX_PROCESSES || target_pid >= MAX_PROCESSES || type >= NS_MAX_TYPES) {
        return -1;
    }
    
    while (__sync_lock_test_and_set(&ns_lock, 1)) {
        asm volatile("pause");
    }
    
    proc_ns[pid].ns_id[type] = proc_ns[target_pid].ns_id[type];
    
    __sync_lock_release(&ns_lock);
    
    return 0;
}

/* 检查命名空间可见性 */
int process_ns_can_see(uint32_t pid1, uint32_t pid2, enum ns_type type) {
    if (pid1 >= MAX_PROCESSES || pid2 >= MAX_PROCESSES || type >= NS_MAX_TYPES) {
        return 0;
    }
    
    return proc_ns[pid1].ns_id[type] == proc_ns[pid2].ns_id[type];
}

/* === 进程综合诊断报告 === */

void process_print_diagnostic_report(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        // println("Invalid PID");
        return;
    }
    
    struct process *proc = &process_table[pid];
    if (proc->state == PROCESS_STATE_UNUSED) {
        // println("Process not found");
        return;
    }
    
    // println("");
    // println("========================================");
    // println("       Process Diagnostic Report        ");
    // println("========================================");
    // println("");
    
    /* 基本信息 */
    // println("--- Basic Information ---");
    // print("PID: ");
    // print_dec(pid);
    // println("");
    
    // print("Name: ");
    // println(proc->name);
    
    // print("State: ");
    // print_dec(proc->state);
    // println("");
    
    // print("Parent PID: ");
    // print_dec(proc->parent_pid);
    // println("");
    
    // print("Threads: ");
    // print_dec(proc->thread_count);
    // println("");
    
    /* 资源限制 */
    // println("");
    // println("--- Resource Limits ---");
    
    uint64_t cur, max;
    if (process_get_rlimit(pid, RLIMIT_CPU, &cur, &max) == 0) {
        // print("CPU Time: ");
        // print_dec(cur);
        // print(" / ");
        // print_dec(max);
        // println("");
    }
    
    if (process_get_rlimit(pid, RLIMIT_NOFILE, &cur, &max) == 0) {
        // print("Open Files: ");
        // print_dec(cur);
        // print(" / ");
        // print_dec(max);
        // println("");
    }
    
    if (process_get_rlimit(pid, RLIMIT_AS, &cur, &max) == 0) {
        // print("Address Space: ");
        // print_dec(cur / (1024 * 1024));
        // print(" MB / ");
        // print_dec(max / (1024 * 1024));
        // println(" MB");
    }
    
    /* 资源使用 */
    // println("");
    // println("--- Resource Usage ---");
    
    struct process_resource_usage usage;
    process_get_usage(pid, &usage);
    
    // print("CPU Time: ");
    // print_dec(usage.cpu_time_total / 1000000);
    // println(" ms");
    
    // print("Voluntary Switches: ");
    // print_dec(usage.voluntary_switches);
    // println("");
    
    // print("Involuntary Switches: ");
    // print_dec(usage.involuntary_switches);
    // println("");
    
    // print("Minor Page Faults: ");
    // print_dec(usage.page_faults_minor);
    // println("");
    
    // print("Major Page Faults: ");
    // print_dec(usage.page_faults_major);
    // println("");
    
    // print("Block Read: ");
    // print_dec(usage.block_read_bytes / 1024);
    // println(" KB");
    
    // print("Block Write: ");
    // print_dec(usage.block_write_bytes / 1024);
    // println(" KB");
    
    // print("Network Recv: ");
    // print_dec(usage.network_bytes_recv / 1024);
    // println(" KB");
    
    // print("Network Sent: ");
    // print_dec(usage.network_bytes_sent / 1024);
    // println(" KB");
    
    /* 命名空间 */
    // println("");
    // println("--- Namespaces ---");
    
    const char *ns_names[] = {"MNT", "UTS", "IPC", "NET", "PID", "USER"};
    for (int i = 0; i < NS_MAX_TYPES; i++) {
        // print(ns_names[i]);
        // print(": ");
        // print_dec(proc_ns[pid].ns_id[i]);
        // println("");
    }
    
    /* 检查点状态 */
    // println("");
    // println("--- Checkpoint ---");
    
    if (checkpoints[pid].valid) {
        // print("Last Checkpoint: ");
        // print(checkpoints[pid].name);
        // print(" @ ");
        // print_dec(checkpoints[pid].timestamp);
        // println("");
    } else {
        // println("No checkpoint");
    }
    
    // println("");
    // println("========================================");
    // println("          End of Report");
    // println("========================================");
    // println("");
}

/* === 全局初始化增强 === */

void task_init_enhanced(void) {
    /* 初始化基础任务管理 */
    task_init();
    
    /* 初始化检查点 */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        checkpoints[i].valid = 0;
    }
    
    /* 初始化命名空间 */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        proc_ns[i].active = 0;
    }
    
    /* 初始化资源统计 */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_reset_usage(i);
    }
    
    // println("[TASK] Enhanced task management initialized");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 进程冻结/解冻支持 (用于休眠等场景)
 * THREAD_FLAG_FROZEN 已在 task.h 中定义
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 冻结任务链表 */
static struct list_head frozen_tasks;
static volatile int frozen_tasks_lock = 0;
static int frozen_list_initialized = 0;

/* 确保冻结链表已初始化 */
static void frozen_list_ensure_init(void) {
    if (!frozen_list_initialized) {
        INIT_LIST_HEAD(&frozen_tasks);
        frozen_list_initialized = 1;
    }
}

/* 冻结单个进程: 设置 FROZEN 标志，将线程从运行队列移除 */
int task_freeze(struct process *proc) {
    if (proc == NULL) return -1;

    struct thread *thread = proc->threads;
    while (thread != NULL) {
        /* 跳过已终止或已冻结的线程 */
        if (thread->state == THREAD_STATE_TERMINATED ||
            thread->state == THREAD_STATE_UNUSED ||
            (thread->flags & THREAD_FLAG_FROZEN)) {
            thread = thread->process_next;
            continue;
        }

        /* 设置冻结标志 */
        thread->flags |= THREAD_FLAG_FROZEN;

        /* 从运行队列移除 */
        if (thread->state == THREAD_STATE_READY && thread->cpu >= 0) {
            extern void scheduler_remove_thread(struct thread *t);
            scheduler_remove_thread(thread);
        }

        /* 将线程状态改为阻塞 */
        if (thread->state == THREAD_STATE_RUNNING) {
            thread->state = THREAD_STATE_BLOCKED;
        }

        thread = thread->process_next;
    }

    /* 加入冻结任务链表 */
    frozen_list_ensure_init();
    while (__sync_lock_test_and_set(&frozen_tasks_lock, 1))
        asm volatile("pause");
    /* 使用进程第一个线程的 cgroup_list 作为链表节点 (复用已有字段) */
    if (proc->threads != NULL) {
        list_add_tail(&proc->threads->cgroup_list, &frozen_tasks);
    }
    __sync_lock_release(&frozen_tasks_lock);

    return 0;
}

/* 解冻单个进程: 清除 FROZEN 标志，将线程重新加入运行队列 */
int task_thaw(struct process *proc) {
    if (proc == NULL) return -1;

    struct thread *thread = proc->threads;
    while (thread != NULL) {
        /* 只处理被冻结的线程 */
        if (!(thread->flags & THREAD_FLAG_FROZEN)) {
            thread = thread->process_next;
            continue;
        }

        /* 清除冻结标志 */
        thread->flags &= ~THREAD_FLAG_FROZEN;

        /* 恢复线程状态并加入运行队列 */
        if (thread->state == THREAD_STATE_BLOCKED) {
            thread->state = THREAD_STATE_READY;
            extern void scheduler_add_thread(struct thread *t);
            scheduler_add_thread(thread);
        }

        thread = thread->process_next;
    }

    /* 从冻结任务链表移除 */
    frozen_list_ensure_init();
    while (__sync_lock_test_and_set(&frozen_tasks_lock, 1))
        asm volatile("pause");
    if (proc->threads != NULL && proc->threads->cgroup_list.next != NULL) {
        list_del(&proc->threads->cgroup_list);
        proc->threads->cgroup_list.next = &proc->threads->cgroup_list;
        proc->threads->cgroup_list.prev = &proc->threads->cgroup_list;
    }
    __sync_lock_release(&frozen_tasks_lock);

    return 0;
}

/* 冻结所有用户进程 (保留内核线程) */
int freeze_all_tasks(void) {
    int frozen = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        struct process *proc = &process_table[i];
        if (proc->state == PROCESS_STATE_UNUSED ||
            proc->state == PROCESS_STATE_ZOMBIE) {
            continue;
        }

        /* 跳过内核进程 (PID 1 的 init 等) */
        if (proc->pid <= 1) continue;

        /* 跳过内核线程 (uid == 0 且无用户栈) */
        struct thread *t = proc->threads;
        if (t != NULL && (t->flags & THREAD_FLAG_KTHREAD)) continue;

        if (task_freeze(proc) == 0) {
            frozen++;
        }
    }

    return frozen;
}

/* 解冻所有冻结的进程 */
int thaw_all_tasks(void) {
    int thawed = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        struct process *proc = &process_table[i];
        if (proc->state == PROCESS_STATE_UNUSED) continue;

        /* 检查进程是否有冻结的线程 */
        struct thread *t = proc->threads;
        if (t != NULL && (t->flags & THREAD_FLAG_FROZEN)) {
            if (task_thaw(proc) == 0) {
                thawed++;
            }
        }
    }

    return thawed;
}
