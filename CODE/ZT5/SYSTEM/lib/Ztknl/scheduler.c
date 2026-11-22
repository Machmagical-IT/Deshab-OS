/*
 * Zratro OS - Per-CPU Scheduler Implementation
 * 
 * 支持 SMP 的多核调度器，带负载均衡
 * 支持 POSIX 实时调度策略 (SCHED_FIFO, SCHED_RR)
 */

#include "scheduler.h"
#include "task.h"
#include "smp.h"
#include "heap.h"
#include "apic.h"
#include "gdt.h"
#include "console.h"
#include "cpu.h"
#include "fb.h"  /* Framebuffer */
#include "cfs.h" /* CFS/EEVDF 红黑树和权重表 */
#include "realtime.h" /* 优先级继承 (rt_thread_effective_prio) */

/* 用户空间 framebuffer 地址（从 main.c 导入） */
extern uint64_t g_user_fb_virt;
#include "string.h"
#include "pmm.h"

/* 串口调试输出函数 (在 console.c 中定义) */
extern void serial_print(const char *str);

/* iretq 帧调试函数 */
void debug_iretq_frame(uint64_t rip, uint64_t cs, uint64_t rflags, 
                       uint64_t rsp, uint64_t ss) {
    serial_print("[IRETQ] RIP=0x");
    char buf[20];
    for (int i = 0; i < 16; i++) buf[i] = "0123456789ABCDEF"[(rip >> (60-i*4)) & 0xF];
    buf[16] = '\0';
    serial_print(buf);
    serial_print(" CS=0x");
    for (int i = 0; i < 4; i++) buf[i] = "0123456789ABCDEF"[(cs >> (12-i*4)) & 0xF];
    buf[4] = '\0';
    serial_print(buf);
    serial_print(" RFLAGS=0x");
    for (int i = 0; i < 16; i++) buf[i] = "0123456789ABCDEF"[(rflags >> (60-i*4)) & 0xF];
    buf[16] = '\0';
    serial_print(buf);
    serial_print("\n[IRETQ] RSP=0x");
    for (int i = 0; i < 16; i++) buf[i] = "0123456789ABCDEF"[(rsp >> (60-i*4)) & 0xF];
    buf[16] = '\0';
    serial_print(buf);
    serial_print(" SS=0x");
    for (int i = 0; i < 4; i++) buf[i] = "0123456789ABCDEF"[(ss >> (12-i*4)) & 0xF];
    buf[4] = '\0';
    serial_print(buf);
    serial_print("\n");
}

/* Per-CPU 运行队列 */
struct runqueue runqueues[MAX_CPUS];

/* 调度锁 */
static volatile int global_sched_lock = 0;

/* Per-CPU 睡眠队列 (消除全局锁争用) */
struct sleep_queue percpu_sleepq[MAX_CPUS];

/* 负载均衡间隔 (ticks) */
#define LOAD_BALANCE_INTERVAL  100

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：实时调度类 (RT Scheduler)
 * 实现 POSIX SCHED_FIFO 和 SCHED_RR 策略
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 实时调度策略 */
#define SCHED_NORMAL    0   /* 普通分时调度 */
#define SCHED_FIFO      1   /* 先进先出实时调度 */
#define SCHED_RR        2   /* 时间片轮转实时调度 */
#define SCHED_BATCH     3   /* 批处理调度 */
/* SCHED_IDLE 已在 task.h 中定义 */

/* 实时优先级范围 (0-99, 数字越大优先级越高) */
#define RT_PRIO_MIN     0
#define RT_PRIO_MAX     99
#define RT_PRIO_COUNT   100

/* 位图数组大小计算 (确保覆盖所有优先级) */
#define RT_BITMAP_SIZE  ((RT_PRIO_COUNT + 31) / 32)  /* = 4, 覆盖 128 位 */

/* 实时运行队列 (Per-CPU) */
struct rt_runqueue {
    struct list_head active[RT_PRIO_COUNT];  /* 每个优先级一个队列 */
    uint32_t bitmap[RT_BITMAP_SIZE];         /* 优先级位图 (修复: 使用正确大小) */
    uint32_t nr_running;                       /* 实时任务数 */
    volatile int lock;
} __attribute__((aligned(64)));

/* 实时运行队列实例 */
static struct rt_runqueue rt_rqs[MAX_CPUS];

/* EEVDF 前向声明 */
void eevdf_init(void);

/* 初始化实时运行队列 */
static void rt_rq_init(struct rt_runqueue *rt_rq) {
    for (int i = 0; i < RT_PRIO_COUNT; i++) {
        INIT_LIST_HEAD(&rt_rq->active[i]);
    }
    for (int i = 0; i < RT_BITMAP_SIZE; i++) {  /* 修复: 使用正确大小 */
        rt_rq->bitmap[i] = 0;
    }
    rt_rq->nr_running = 0;
    rt_rq->lock = 0;
}

/* 实时队列入队 */
static void rt_enqueue(struct rt_runqueue *rt_rq, struct thread *thread) {
    /* 使用有效优先级 (考虑 PI 提升) */
    int prio = rt_thread_effective_prio(thread);
    if (prio < RT_PRIO_MIN) prio = RT_PRIO_MIN;
    if (prio > RT_PRIO_MAX) prio = RT_PRIO_MAX;
    
    /* 添加到对应优先级队列尾部 */
    list_add_tail(&thread->run_list, &rt_rq->active[prio]);
    
    /* 设置位图 */
    rt_rq->bitmap[prio / 32] |= (1U << (prio % 32));
    rt_rq->nr_running++;
}

/* 实时队列出队 */
static void rt_dequeue(struct rt_runqueue *rt_rq, struct thread *thread) {
    /* 使用有效优先级 (考虑 PI 提升)，与 rt_enqueue 保持一致 */
    int prio = rt_thread_effective_prio(thread);
    /* 边界检查：与 rt_enqueue 保持一致 */
    if (prio < RT_PRIO_MIN) prio = RT_PRIO_MIN;
    if (prio > RT_PRIO_MAX) prio = RT_PRIO_MAX;
    
    list_del(&thread->run_list);
    
    /* 如果该优先级队列空了，清除位图 */
    if (list_empty(&rt_rq->active[prio])) {
        rt_rq->bitmap[prio / 32] &= ~(1U << (prio % 32));
    }
    
    if (rt_rq->nr_running > 0) {
        rt_rq->nr_running--;
    }
}

/* 使用位图快速找到最高优先级任务 - O(1) 算法 */
static int rt_find_highest_prio(struct rt_runqueue *rt_rq) __hot;
static int rt_find_highest_prio(struct rt_runqueue *rt_rq) {
    /* 从高到低扫描位图 - 使用 RT_BITMAP_SIZE 确保覆盖所有位 */
    for (int i = RT_BITMAP_SIZE - 1; i >= 0; i--) {
        uint32_t bits = rt_rq->bitmap[i];
        if (unlikely(bits == 0)) continue;
        
        /* 使用 BSR 指令找到最高位 */
        int bit;
        asm volatile("bsr %1, %0" : "=r"(bit) : "r"(bits));
        return i * 32 + bit;
    }
    return -1;  /* 没有实时任务 */
}

/* 实时任务选择 */
static struct thread *rt_pick_next(struct rt_runqueue *rt_rq) __hot;
static struct thread *rt_pick_next(struct rt_runqueue *rt_rq) {
    int prio = rt_find_highest_prio(rt_rq);
    if (unlikely(prio < 0)) return NULL;
    
    if (likely(!list_empty(&rt_rq->active[prio]))) {
        return list_entry(rt_rq->active[prio].next, struct thread, run_list);
    }
    return NULL;
}

/* 实时任务时间片处理 (SCHED_RR) */
static void rt_tick(struct rt_runqueue *rt_rq, struct thread *thread) {
    if (thread->sched_policy == SCHED_RR) {
        thread->rt_time_slice--;
        
        if (thread->rt_time_slice <= 0) {
            /* 时间片用完，重新排队 */
            thread->rt_time_slice = thread->default_slice;
            
            rt_dequeue(rt_rq, thread);
            rt_enqueue(rt_rq, thread);
            
            thread->flags |= THREAD_FLAG_NEED_RESCHED;
        }
    }
    /* SCHED_FIFO 不需要处理时间片 */
}

/* 获取当前 CPU 的实时队列 */
static inline struct rt_runqueue *this_rt_rq(void) {
    return &rt_rqs[smp_processor_id()];
}

/* 实时队列锁 */
static inline void rt_rq_lock(struct rt_runqueue *rt_rq) {
    while (__sync_lock_test_and_set(&rt_rq->lock, 1)) {
        asm volatile("pause");
    }
}

static inline void rt_rq_unlock(struct rt_runqueue *rt_rq) {
    __sync_lock_release(&rt_rq->lock);
}

/* ========== 自旋锁 ========== */

static inline void sched_spin_lock(volatile int *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        asm volatile("pause");
    }
}

static inline void sched_spin_unlock(volatile int *lock) {
    __sync_lock_release(lock);
}

/* ========== 运行队列操作 ========== */

/* 初始化运行队列 */
static void runqueue_init(struct runqueue *rq) {
    for (int i = 0; i < SCHED_PRIORITY_LEVELS; i++) {
        INIT_LIST_HEAD(&rq->queues[i]);
        rq->nr_running[i] = 0;
    }
    /* 初始化红黑树时间线 */
    rq->tasks_timeline.rb_node = NULL;
    rq->tasks_timeline.rb_leftmost = NULL;
    rq->tasks_timeline.nr_nodes = 0;
    rq->rb_leftmost = NULL;
    rq->nr_total = 0;
    rq->load_weight = 0;
    rq->last_balance = 0;
    rq->curr = NULL;
    rq->lock = 0;
}

/* 入队线程 (同时维护链表和红黑树) */
static void enqueue_thread(struct runqueue *rq, struct thread *thread) {
    int prio = thread->priority;
    if (prio < 0) prio = 0;
    if (prio >= SCHED_PRIORITY_LEVELS) prio = SCHED_PRIORITY_LEVELS - 1;

    /* 插入优先级链表 (保留向后兼容) */
    list_add_tail(&thread->run_list, &rq->queues[prio]);
    rq->nr_running[prio]++;
    rq->nr_total++;
    rq->load_weight += (1 << prio);

    /* 插入红黑树 (按 vruntime 排序，O(log n)) */
    struct sched_entity *se = &thread->sched_entity;
    se->run_node.key = se->vruntime;
    rb_insert(&rq->tasks_timeline, &se->run_node);
    /* 更新 leftmost 缓存 */
    if (rq->rb_leftmost == NULL || se->vruntime < ((struct sched_entity *)container_of(rq->rb_leftmost, struct sched_entity, run_node))->vruntime) {
        rq->rb_leftmost = &se->run_node;
    }
    se->on_rq = 1;

    thread->state = THREAD_STATE_READY;
    thread->cpu = smp_processor_id();
}

/* 出队线程 (同时从链表和红黑树移除) */
static void dequeue_thread(struct runqueue *rq, struct thread *thread) {
    if (thread == NULL) {
        serial_print("[SCHED] dequeue_thread: NULL thread!\n");
        return;
    }
    
    int prio = thread->priority;
    if (prio < 0) prio = 0;
    if (prio >= SCHED_PRIORITY_LEVELS) prio = SCHED_PRIORITY_LEVELS - 1;
    
    /* 从优先级链表移除 */
    list_del(&thread->run_list);
    
    /* 更新统计，防止下溢 */
    if (rq->nr_running[prio] > 0) {
        rq->nr_running[prio]--;
    }
    if (rq->nr_total > 0) {
        rq->nr_total--;
    }
    
    uint64_t weight = (1ULL << prio);
    if (rq->load_weight >= weight) {
        rq->load_weight -= weight;
    } else {
        rq->load_weight = 0;  /* 防止下溢 */
    }

    /* 从红黑树移除 (O(log n)) */
    struct sched_entity *se = &thread->sched_entity;
    if (se->on_rq) {
        /* 如果移除的是 leftmost 节点，需要更新缓存 */
        if (rq->rb_leftmost == &se->run_node) {
            struct rb_node *next = rb_next(&se->run_node);
            rq->rb_leftmost = next;
        }
        rb_erase(&rq->tasks_timeline, &se->run_node);
        se->on_rq = 0;
    }
    
    thread->cpu = -1;
}

/* 选择下一个线程 (优先使用红黑树 O(1)，回退到链表) */
static struct thread *pick_next_thread(struct runqueue *rq) {
    /* 快速路径：使用红黑树 leftmost 缓存，O(1) 取最小 vruntime */
    if (rq->rb_leftmost != NULL) {
        struct sched_entity *se = container_of(rq->rb_leftmost, struct sched_entity, run_node);
        struct thread *thread = se->thread;
        if (thread != NULL && thread->state != THREAD_STATE_TERMINATED) {
            return thread;
        }
    }

    /* 回退路径：从红黑树遍历查找非终止线程 */
    struct rb_node *node = rb_first(&rq->tasks_timeline);
    while (node != NULL) {
        struct sched_entity *se = container_of(node, struct sched_entity, run_node);
        struct thread *thread = se->thread;
        if (thread != NULL && thread->state != THREAD_STATE_TERMINATED) {
            /* 更新 leftmost 缓存 */
            rq->rb_leftmost = node;
            return thread;
        }
        node = rb_next(node);
    }

    /* 最终回退：链表遍历 (兼容性保底) */
    for (int i = SCHED_PRIORITY_LEVELS - 1; i >= 0; i--) {
        struct list_head *entry;
        list_for_each(entry, &rq->queues[i]) {
            struct thread *thread = list_entry(entry, struct thread, run_list);
            /* 跳过已终止的线程 */
            if (thread->state != THREAD_STATE_TERMINATED) {
                return thread;
            } else {
                serial_print("[SCHED] pick_next_thread: skipping tid=");
                char buf[16]; int p = 0;
                uint32_t v = thread->tid;
                if (v == 0) buf[p++] = '0';
                else { char t[16]; int tp = 0; while (v > 0) { t[tp++] = '0' + (v % 10); v /= 10; } while (tp > 0) buf[p++] = t[--tp]; }
                buf[p] = '\0';
                serial_print(buf);
                serial_print(" (TERMINATED)\n");
            }
        }
    }
    serial_print("[SCHED] pick_next_thread: no runnable thread found\n");
    return NULL;
}

/* ========== 初始化 ========== */

void scheduler_init(void) {
    // println("  [DEBUG] scheduler_init() starting...");

    // print("  [DEBUG] runqueues addr: 0x");
    // print_hex((uint64_t)runqueues);
    // println("");

    // print("  [DEBUG] sizeof(runqueue): ");
    // print_dec(sizeof(struct runqueue));
    // println(" bytes");

    /* 初始化 Per-CPU 睡眠队列 */
    for (int i = 0; i < MAX_CPUS; i++) {
        INIT_LIST_HEAD(&percpu_sleepq[i].list);
        percpu_sleepq[i].lock = 0;
    }

    /* 测试 this_cpu() 在 scheduler_init 中是否正常 */
    // print("  [DEBUG] Testing this_cpu() in scheduler_init...");
    struct cpu_local *cpu = this_cpu();
    if (cpu == NULL) {
        // println("");
        // println("  [ERROR] this_cpu() returned NULL!");
        // println("  [ERROR] SMP GS base not properly set!");
    } else {
        // print(" OK (cpu_id=");
        // print_dec(cpu->cpu_id);
        // println(")");
    }

    /* 初始化所有 CPU 的运行队列 */
    // print("  [DEBUG] Initializing runqueues for ");
    // print_dec(MAX_CPUS);
    // println(" CPUs...");
    for (int i = 0; i < MAX_CPUS; i++) {
        runqueue_init(&runqueues[i]);
        rt_rq_init(&rt_rqs[i]);  /* 初始化实时队列 */
    }
    // println("  [DEBUG] runqueues initialized");

    /* 初始化 EEVDF 调度器 */
    eevdf_init();
    
    // print("    Max CPUs: ");
    // print_dec(MAX_CPUS);
    // println("");
    // print("    Time Slice: ");
    // print_dec(DEFAULT_TIME_SLICE);
    // println(" ms");
    // print("    RT Priority Range: 0-99");
    // println("");
    
    // println("  [DEBUG] scheduler_init() completed");
}

void scheduler_init_cpu(int cpu_id) {
    // print("  [DEBUG] scheduler_init_cpu(");
    // print_dec(cpu_id);
    // println(") starting...");
    
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
        // println("  [ERROR] Invalid cpu_id!");
        return;
    }
    
    struct runqueue *rq = &runqueues[cpu_id];
    runqueue_init(rq);
    
    /* 创建空闲线程 */
    struct cpu_local *cpu = &cpu_data[cpu_id];
    // print("  [DEBUG] cpu_data[");
    // print_dec(cpu_id);
    // print("] addr: 0x");
    // print_hex((uint64_t)cpu);
    // println("");
    
    if (cpu->idle_thread == NULL) {
        // print("  [DEBUG] Creating idle thread for CPU ");
        // print_dec(cpu_id);
        // println("...");
        
        cpu->idle_thread = thread_create_kernel("idle", idle_task, NULL);
        if (cpu->idle_thread) {
            cpu->idle_thread->priority = PRIORITY_IDLE;
            cpu->idle_thread->time_slice = 1;
            cpu->idle_thread->default_slice = 1;
            cpu->idle_thread->state = THREAD_STATE_RUNNING;
            cpu->idle_thread->cpu = cpu_id;
            
            // print("  [DEBUG] idle_thread created: 0x");
            // print_hex((uint64_t)cpu->idle_thread);
            // println("");
        } else {
            // println("  [ERROR] Failed to create idle thread!");
        }
    } else {
        // print("  [DEBUG] idle_thread already exists: 0x");
        // print_hex((uint64_t)cpu->idle_thread);
        // println("");
    }
    
    /* 设置当前线程为空闲线程，确保定时器中断时 rq->curr 有效 */
    if (cpu->idle_thread != NULL) {
        rq->curr = cpu->idle_thread;
        cpu->current_thread = cpu->idle_thread;
        // print("  [DEBUG] Set current_thread to idle: 0x");
        // print_hex((uint64_t)cpu->current_thread);
        // println("");
    }
    
    // print("[SCHED] CPU ");
    // print_dec(cpu_id);
    // println(" runqueue initialized");
}

/* ========== 线程调度 ========== */

void scheduler_add_thread(struct thread *thread) {
    if (thread == NULL) return;
    
    int target_cpu;
    
    /* 实时线程处理 */
    if (thread->sched_policy == SCHED_FIFO || thread->sched_policy == SCHED_RR) {
        /* 实时任务绑定到当前 CPU 或第一个可用 CPU */
        target_cpu = smp_processor_id();
        
        struct rt_runqueue *rt_rq = &rt_rqs[target_cpu];
        
        rt_rq_lock(rt_rq);
        
        /* 初始化实时时间片 */
        if (thread->sched_policy == SCHED_RR && thread->rt_time_slice == 0) {
            thread->rt_time_slice = DEFAULT_TIME_SLICE;
        }
        
        rt_enqueue(rt_rq, thread);
        thread->state = THREAD_STATE_READY;
        thread->cpu = target_cpu;
        
        rt_rq_unlock(rt_rq);
        
        /* 实时任务可能抢占当前任务 */
        if (target_cpu != smp_processor_id()) {
            smp_send_reschedule(target_cpu);
        }
        return;
    }
    
    /* 选择负载最小的 CPU (考虑 CPU 亲和性掩码) */
    target_cpu = -1;
    uint32_t affinity = thread->cpu_affinity;
    /* 如果亲和性掩码为 0，允许所有 CPU */
    if (affinity == 0) {
        affinity = (1U << smp_cpu_count) - 1;
    }
    uint32_t min_load = (uint32_t)-1;

    for (int i = 0; i < smp_cpu_count; i++) {
        /* 检查 CPU 是否在亲和性掩码中 */
        if (!((1U << i) & affinity)) continue;
        if (runqueues[i].nr_total < min_load) {
            min_load = runqueues[i].nr_total;
            target_cpu = i;
        }
    }

    /* 如果亲和性掩码中没有可用 CPU，回退到负载最小的 */
    if (target_cpu < 0) {
        target_cpu = 0;
        min_load = runqueues[0].nr_total;
        for (int i = 1; i < smp_cpu_count; i++) {
            if (runqueues[i].nr_total < min_load) {
                min_load = runqueues[i].nr_total;
                target_cpu = i;
            }
        }
    }
    
    struct runqueue *rq = &runqueues[target_cpu];
    
    rq_lock(rq);
    enqueue_thread(rq, thread);
    rq_unlock(rq);
    
    /* 如果添加到其他 CPU，发送 IPI */
    if (target_cpu != smp_processor_id()) {
        smp_send_reschedule(target_cpu);
    }
}

void scheduler_remove_thread(struct thread *thread) {
    if (thread == NULL || thread->cpu < 0) return;
    
    struct runqueue *rq = &runqueues[thread->cpu];
    
    rq_lock(rq);
    if (thread->state == THREAD_STATE_READY) {
        dequeue_thread(rq, thread);
    }
    rq_unlock(rq);
}

struct thread *scheduler_pick_next(void) {
    struct runqueue *rq = this_rq();
    return pick_next_thread(rq);
}

/* 上下文切换 (汇编实现) */
extern void context_switch(struct cpu_context *old, struct cpu_context *new);

/* 调度主函数 */
void schedule(void) __hot;
void schedule(void) {
    struct cpu_local *cpu = this_cpu();
    struct runqueue *rq = this_rq();
    struct rt_runqueue *rt_rq = this_rt_rq();
    
    /* 禁用中断，防止调度过程中被中断重入 */
    cpu_disable_interrupts();
    
    /* 检查是否可调度 - 快速退出路径 */
    if (unlikely(cpu->preempt_count > 0 || cpu->spinlock_count > 0)) {
        cpu_enable_interrupts();
        return;
    }
    
    struct thread *prev = rq->curr;
    struct thread *next = NULL;
    int is_rt = 0;
    
    /* 优先检查实时任务 - 热路径 */
    rt_rq_lock(rt_rq);
    next = rt_pick_next(rt_rq);
    if (likely(next != NULL)) {
        is_rt = 1;
        rt_dequeue(rt_rq, next);
    }
    rt_rq_unlock(rt_rq);
    
    /* 没有实时任务，使用普通调度 */
    if (unlikely(next == NULL)) {
        rq_lock(rq);
        next = pick_next_thread(rq);
        
        /* 如果没有就绪线程，使用空闲线程 */
        if (unlikely(next == NULL)) {
            next = cpu->idle_thread;
        }
        
        /* 关键修复：不要在这里 dequeue，等到确认要切换后再 dequeue */
        /* 检查是否需要提前返回 */
        if (unlikely(next == NULL || next == prev)) {
            /* 如果 dequeue 了 idle_thread（不可能），或者 next==prev，直接返回 */
            rq_unlock(rq);
            cpu_enable_interrupts();
            return;
        }
        
        /* 不要 dequeue idle_thread，它不在运行队列中 */
        if (next == cpu->idle_thread) {
            rq_unlock(rq);
        } else {
            /* 确认要切换到新线程，才从运行队列中移除 */
            dequeue_thread(rq, next);
            rq_unlock(rq);
        }
    } else {
        /* 实时任务：也需要检查 next == prev */
        if (unlikely(next == prev)) {
            /* 需要重新入队实时任务 */
            rt_rq_lock(rt_rq);
            rt_enqueue(rt_rq, next);
            rt_rq_unlock(rt_rq);
            
            cpu_enable_interrupts();
            return;
        }
    }

    /* 更新状态 */
    rq_lock(rq);
    if (likely(prev && prev != cpu->idle_thread && !is_rt)) {
        if (prev->state == THREAD_STATE_RUNNING) {
            prev->state = THREAD_STATE_READY;
            enqueue_thread(rq, prev);
        }
    }
    rq_unlock(rq);
    
    next->state = THREAD_STATE_RUNNING;
    if (!is_rt && next->sched_policy != SCHED_FIFO && next->sched_policy != SCHED_RR) {
        next->time_slice = next->default_slice;
    }
    
    rq->curr = next;
    
    /* 更新 CPU 数据 */
    cpu->current_thread = next;
    
    cpu->schedule_count++;
    
    /* 更新 TSS 内核栈
     * 对于用户线程：使用 kernel_stack（内核栈顶）
     *   当用户态发生中断时，CPU 从 RSP0 开始向下压入 iretq 帧
     * 对于内核线程：使用 context.rsp（schedule() 的返回点）
     * 对于 Idle 线程：使用 context.rsp */
    if (likely(next->pid > 0 && next->kernel_stack != 0)) {
        /* 用户线程：设置 RSP0 为内核栈顶 */
        extern struct process *process_get_by_pid(uint32_t pid);
        struct process *proc = process_get_by_pid(next->pid);
        if (proc && proc->space) {
            tss_set_kernel_stack(next->kernel_stack);
        } else if (likely(next->context.rsp != 0)) {
            /* 内核线程 */
            tss_set_kernel_stack(next->context.rsp);
        }
    } else if (likely(next->context.rsp != 0)) {
        /* Idle 线程 */
        tss_set_kernel_stack(next->context.rsp);
    }
    
    /* 切换进程地址空间 */
    if (next->pid > 0) {
        extern struct process *process_get_by_pid(uint32_t pid);
        struct process *next_proc = process_get_by_pid(next->pid);
        struct process *prev_proc = prev ? process_get_by_pid(prev->pid) : NULL;

        if (next_proc && next_proc->space) {
            if (prev_proc == NULL || prev_proc != next_proc) {
                /* 使用 vmm_switch_space 来正确切换地址空间 */
                extern void vmm_switch_space(struct address_space *space);
                vmm_switch_space(next_proc->space);
            }
        }
    }
    
    /* 执行上下文切换 */
    if (likely(prev)) {
        context_switch(&prev->context, &next->context);
        
        /* context_switch 返回后，prev 线程被重新调度 */
        /* 中断在 context_asm.S 中保存 RFLAGS 时已强制 IF=1，
         * iretq 恢复时自动启用中断 */
    }
}

/* 主动让出 CPU */
void yield(void) {
    struct thread *current = this_cpu()->current_thread;
    if (current) {
        current->flags |= THREAD_FLAG_NEED_RESCHED;
        schedule();
    }
}

/* ========== 定时器中断 ========== */

void scheduler_tick(struct interrupt_frame *frame) {
    (void)frame;
    
    struct cpu_local *cpu = this_cpu();
    struct runqueue *rq = this_rq();
    
    /* 增加 tick 计数 */
    increment_tick_count();

    /* 诊断输出: 确认 scheduler_tick 被调用 (限制前 10 次) */
    {
        static int _tick_dbg_cnt = 0;
        if (_tick_dbg_cnt < 10) {
            _tick_dbg_cnt++;
            int _cpu = smp_processor_id();
            serial_print("[TICK] cpu=");
            { char _nb[8]; int _np=0; int _v=_cpu; if(_v==0){_nb[_np++]='0';}else{char _tb[8];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
            serial_print(" tick=");
            { char _nb[16]; int _np=0; unsigned long _v=(unsigned long)get_tick_count(); if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
            serial_print("\n");
        }
    }
    
    /* EOI 由 isr_handler 统一发送，此处不再重复 */
    
    /* 检查所有 CPU 的睡眠线程 (修复: 线程可能在任意 CPU 的睡眠队列上) */
    uint64_t now = get_tick_count();

    for (int scan_cpu = 0; scan_cpu < smp_cpu_count; scan_cpu++) {
        struct sleep_queue *sq = &percpu_sleepq[scan_cpu];

        while (__sync_lock_test_and_set(&sq->lock, 1)) {
            asm volatile("pause");
        }

        struct list_head *pos, *n;
        list_for_each_safe(pos, n, &sq->list) {
            struct thread *t = list_entry(pos, struct thread, sleep_list);
            if (t->wake_time <= now) {
                list_del_init(&t->sleep_list);
                t->state = THREAD_STATE_READY;

                /* 诊断输出: 记录线程唤醒 (限制前 20 次) */
                {
                    static int _wake_dbg_cnt = 0;
                    if (_wake_dbg_cnt < 20) {
                        _wake_dbg_cnt++;
                        serial_print("[WAKE] tid=");
                        { char _nb[16]; int _np=0; long _v=(long)t->tid; if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
                        serial_print(" from_cpu=");
                        { char _nb[8]; int _np=0; int _v=scan_cpu; if(_v==0){_nb[_np++]='0';}else{char _tb[8];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
                        serial_print(" now=");
                        { char _nb[16]; int _np=0; unsigned long _v=(unsigned long)now; if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
                        serial_print("\n");
                    }
                }

                __sync_lock_release(&sq->lock);
                scheduler_add_thread(t);
                while (__sync_lock_test_and_set(&sq->lock, 1)) {
                    asm volatile("pause");
                }
            }
        }

        __sync_lock_release(&sq->lock);
    }
    
    /* 更新当前线程时间片 */
    struct thread *current = rq->curr;
    if (current && current->priority != PRIORITY_IDLE) {
        /* 检查是否为实时任务 */
        if (current->sched_policy == SCHED_RR || current->sched_policy == SCHED_FIFO) {
            /* 实时任务时间片处理 */
            struct rt_runqueue *rt_rq = this_rt_rq();
            rt_tick(rt_rq, current);
        } else {
            /* 普通任务时间片处理 */
            current->time_slice--;
            
            if (current->time_slice <= 0) {
                current->flags |= THREAD_FLAG_NEED_RESCHED;
            }
        }
        current->total_run_time++;
    }
    
    /* 负载均衡检查 */
    if (now - rq->last_balance >= LOAD_BALANCE_INTERVAL) {
        rq->last_balance = now;
        /* 标记需要负载均衡 */
    }
    
    /* 检查是否需要调度 */
    if (likely(current) && (current->flags & THREAD_FLAG_NEED_RESCHED)) {
        if (likely(cpu->preempt_count == 0 && cpu->spinlock_count == 0)) {
            /* 关键修复: 在调用 schedule() 前先发送 EOI
             * 因为 schedule() 可能切换到其他线程，而 isr_handler 中的 EOI
             * 要等中断处理程序返回后才发送。如果不提前发送 EOI，
             * APIC 不会产生新的中断，导致定时器永远不再触发 */
            extern void apic_eoi(void);
            apic_eoi();
            schedule();
            return; /* schedule() 返回后不再由 isr_handler 发 EOI (已发送) */
        }
    }
}

/* ========== 负载均衡 ========== */

void load_balance(int cpu) {
    struct runqueue *src_rq = &runqueues[cpu];
    
    /* 找到最忙和最闲的 CPU */
    int busiest_cpu = cpu;  /* 初始化为当前 CPU */
    uint32_t max_load = src_rq->nr_total;
    
    int idlest_cpu = cpu;  /* 初始化为当前 CPU */
    uint32_t min_load = src_rq->nr_total;
    
    for (int i = 0; i < smp_cpu_count; i++) {
        uint32_t load = runqueues[i].nr_total;
        if (load > max_load) {
            max_load = load;
            busiest_cpu = i;
        }
        if (load < min_load) {
            min_load = load;
            idlest_cpu = i;
        }
    }
    
    /* 如果差距不够大，不迁移 */
    if (busiest_cpu == idlest_cpu || max_load <= min_load + 1) {
        return;
    }
    
    /* 只从最忙的 CPU 迁移到当前 CPU */
    if (idlest_cpu != cpu) {
        return;  /* 不是当前 CPU 最闲，不进行负载均衡 */
    }
    
    /* 迁移一个线程 */
    struct runqueue *busy_rq = &runqueues[busiest_cpu];
    
    rq_lock(busy_rq);
    
    /* 从最低优先级队列选择线程 (不包括空闲线程) */
    struct thread *migrate = NULL;
    for (int i = 0; i < SCHED_PRIORITY_LEVELS - 1; i++) {
        if (!list_empty(&busy_rq->queues[i])) {
            migrate = list_entry(busy_rq->queues[i].next, struct thread, run_list);
            break;
        }
    }
    
    if (migrate) {
        dequeue_thread(busy_rq, migrate);
    }
    
    rq_unlock(busy_rq);
    
    if (migrate) {
        /* 添加到当前 CPU */
        rq_lock(src_rq);
        enqueue_thread(src_rq, migrate);
        rq_unlock(src_rq);
    }
}

void trigger_load_balance(int cpu) {
    load_balance(cpu);
}

int migrate_thread(struct thread *thread, int target_cpu) {
    if (thread == NULL || target_cpu < 0 || target_cpu >= smp_cpu_count) {
        return -1;
    }
    
    /* 检查目标 CPU 是否在线程的亲和性掩码中 */
    if (thread->cpu_affinity != 0 && !((1U << target_cpu) & thread->cpu_affinity)) {
        return -1;  /* 目标 CPU 不在亲和性掩码中 */
    }
    
    int src_cpu = thread->cpu;
    if (src_cpu < 0 || src_cpu == target_cpu) {
        return -1;
    }
    
    /* 从源 CPU 移除 */
    struct runqueue *src_rq = &runqueues[src_cpu];
    rq_lock(src_rq);
    
    if (thread->state == THREAD_STATE_READY) {
        dequeue_thread(src_rq, thread);
    }
    
    rq_unlock(src_rq);
    
    /* 添加到目标 CPU */
    struct runqueue *dst_rq = &runqueues[target_cpu];
    rq_lock(dst_rq);
    enqueue_thread(dst_rq, thread);
    rq_unlock(dst_rq);
    
    /* 通知目标 CPU */
    smp_send_reschedule(target_cpu);
    
    return 0;
}

/* ========== 等待队列 ========== */

void sleep_on(wait_queue_head_t *wq) {
    struct thread *current = this_cpu()->current_thread;
    if (current == NULL) return;
    
    /* 创建等待队列项 */
    struct wait_queue wait = {
        .thread = current,
    };
    INIT_LIST_HEAD(&wait.list);
    
    /* 加入等待队列 */
    sched_spin_lock(&wq->lock);
    list_add_tail(&wait.list, &wq->list);
    current->state = THREAD_STATE_BLOCKED;
    sched_spin_unlock(&wq->lock);
    
    /* 调度 */
    schedule();
}

void wake_up(wait_queue_head_t *wq) {
    sched_spin_lock(&wq->lock);
    
    while (!list_empty(&wq->list)) {
        struct list_head *first = wq->list.next;
        struct wait_queue *wait = list_entry(first, struct wait_queue, list);
        struct thread *thread = wait->thread;
        
        list_del(&wait->list);
        thread->state = THREAD_STATE_READY;
        scheduler_add_thread(thread);
    }
    
    sched_spin_unlock(&wq->lock);
}

void wake_up_process(struct thread *thread) {
    if (thread == NULL) return;
    
    thread->state = THREAD_STATE_READY;
    scheduler_add_thread(thread);
}

/* ========== 空闲任务 ========== */

void idle_task(void *arg) {
    (void)arg;

    while (1) {
        /* 确保中断启用 — schedule() 切换到 idle 时可能禁用了中断，
         * 如果中断未启用，hlt 将永远无法被定时器唤醒 */
        sti();

        struct runqueue *rq = this_rq();

        if (rq->nr_total > 0) {
            schedule();
        } else {
            struct cpu_local *cpu = this_cpu();
            struct thread *cur = cpu->current_thread;
            if (cur && cur->pid > 0 && cur != cpu->idle_thread &&
                cur->state == THREAD_STATE_RUNNING) {
                cur->state = THREAD_STATE_READY;
                rq_lock(rq);
                enqueue_thread(rq, cur);
                rq_unlock(rq);
                schedule();
            }
        }

        /* 等待中断 - CPU 进入低功耗状态 */
        sti();  /* 再次确保中断启用，防止 schedule() 返回后中断被禁用 */
        asm volatile("hlt");
    }
}

/* ========== 调度器启动 ========== */

void scheduler_start(void) {
    /* 注意：运行队列已在 scheduler_init() 中初始化，不再重新初始化！
     * 只需要设置当前线程和进入空闲任务 */
    struct cpu_local *cpu = &cpu_data[0];
    struct runqueue *rq = &runqueues[0];
    
    serial_print("[SCHED] scheduler_start() called\n");
    
    /* 调试：打印运行队列状态 */
    print("[SCHED] Runqueue nr_total = ");
    char buf[16];
    int n = rq->nr_total;
    int pos = 0;
    if (n == 0) buf[pos++] = '0';
    else {
        char tmp[16];
        int t = 0;
        while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
        while (t > 0) buf[pos++] = tmp[--t];
    }
    buf[pos] = '\0';
    println(buf);
    
    /* 确保 idle 线程存在 */
    if (cpu->idle_thread == NULL) {
        serial_print("[SCHED] Creating idle thread...\n");
        cpu->idle_thread = thread_create_kernel("idle", idle_task, NULL);
        if (cpu->idle_thread) {
            cpu->idle_thread->priority = PRIORITY_IDLE;
            cpu->idle_thread->time_slice = 1;
            cpu->idle_thread->default_slice = 1;
            cpu->idle_thread->state = THREAD_STATE_RUNNING;
            cpu->idle_thread->cpu = 0;
        }
    }
    
    /* 设置当前线程为空闲线程 */
    if (cpu->idle_thread != NULL) {
        rq->curr = cpu->idle_thread;
        cpu->current_thread = cpu->idle_thread;
    }
    
    serial_print("[SCHED] Enabling interrupts...\n");
    /* 启用中断 */
    sti();
    
    serial_print("[SCHED] Entering idle_task...\n");
    /* 进入空闲任务循环 */
    idle_task(NULL);
}

/* ========== 统计 ========== */

void scheduler_stats(void) {
    // println("=== Scheduler Statistics ===");
    
    for (int i = 0; i < smp_cpu_count; i++) {
        struct runqueue *rq = &runqueues[i];
        
        // print("CPU ");
        // print_dec(i);
        // print(": running=");
        // print_dec(rq->nr_total);
        // print(" curr=");
        if (rq->curr) {
            // print(rq->curr->name);
        } else {
            // print("(none)");
        }
        // println("");
    }
}

void scheduler_dump_all(void) {
    // println("=== All Runqueues ===");
    
    for (int cpu = 0; cpu < smp_cpu_count; cpu++) {
        struct runqueue *rq = &runqueues[cpu];
        
        // print("CPU ");
        // print_dec(cpu);
        // print(" (total=");
        // print_dec(rq->nr_total);
        // println("):");
        
        for (int prio = SCHED_PRIORITY_LEVELS - 1; prio >= 0; prio--) {
            if (rq->nr_running[prio] > 0) {
                // print("  Priority ");
                // print_dec(prio);
                // print(": ");
                // print_dec(rq->nr_running[prio]);
                // println(" threads");
            }
        }
    }
}

/* 设置线程优先级 */
void set_thread_priority(struct thread *thread, int priority) {
    if (thread == NULL) return;
    if (priority < 0) priority = 0;
    if (priority >= SCHED_PRIORITY_LEVELS) priority = SCHED_PRIORITY_LEVELS - 1;
    
    /* 如果线程在队列中，需要重新入队 */
    if (thread->state == THREAD_STATE_READY && thread->cpu >= 0) {
        struct runqueue *rq = &runqueues[thread->cpu];
        rq_lock(rq);
        dequeue_thread(rq, thread);
        thread->priority = priority;
        enqueue_thread(rq, thread);
        rq_unlock(rq);
    } else {
        thread->priority = priority;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：CPU 亲和性与 NUMA 感知调度
 * 
 * 特性：
 * - CPU 亲和性绑定
 * - NUMA 节点感知
 * - 缓存亲和性优化
 * - 负载感知迁移
 * ═══════════════════════════════════════════════════════════════════════════ */

/* NUMA 节点配置 */
#define MAX_NUMA_NODES     4
#define MAX_CPUS_PER_NODE  8

struct numa_node {
    int node_id;
    uint32_t cpu_mask;           /* 该节点的 CPU 位掩码 */
    uint64_t memory_size;        /* 本地内存大小 */
    uint64_t free_memory;        /* 可用内存 */
    uint32_t distance[MAX_NUMA_NODES];  /* 节点距离矩阵 */
};

/* CPU 缓存亲和性信息 */
struct cpu_cache_info {
    uint32_t l1_size;            /* L1 缓存大小 */
    uint32_t l2_size;            /* L2 缓存大小 */
    uint32_t l3_size;            /* L3 缓存大小 */
    uint32_t l1_line_size;       /* L1 缓存行大小 */
    uint32_t l2_line_size;       /* L2 缓存行大小 */
    uint32_t cores_sharing_l3;   /* 共享 L3 的核心数 */
};

/* NUMA 节点数组 */
static struct numa_node numa_nodes2[MAX_NUMA_NODES];
static int numa_node_count = 1;

/* CPU 到 NUMA 节点映射 */
static int cpu_to_node[MAX_CPUS];

/* CPU 缓存信息 */
static struct cpu_cache_info cpu_cache[MAX_CPUS];

/* 线程亲和性历史 (用于缓存亲和性决策) */
struct affinity_history {
    int last_cpu;
    int preferred_cpu;
    uint32_t cpu_usage_mask;     /* 历史使用的 CPU */
    uint64_t last_run[MAX_CPUS]; /* 每个 CPU 的最后运行时间 */
};

/* 获取线程的 NUMA 节点 */
static inline int thread_get_numa_node(struct thread *thread) {
    if (thread->cpu < 0) return 0;
    return cpu_to_node[thread->cpu];
}

/* 获取 NUMA 节点的 CPU 掩码 */
static inline uint32_t numa_get_cpu_mask(int node) {
    if (node < 0 || node >= numa_node_count) return 0;
    return numa_nodes2[node].cpu_mask;
}

/* 初始化 NUMA 配置 */
void numa_init(void) {
    /* 简化：假设单 NUMA 节点 */
    numa_node_count = 1;
    
    numa_nodes2[0].node_id = 0;
    numa_nodes2[0].cpu_mask = (1U << smp_cpu_count) - 1;
    numa_nodes2[0].distance[0] = 0;
    
    /* 所有 CPU 都属于节点 0 */
    for (int i = 0; i < smp_cpu_count; i++) {
        cpu_to_node[i] = 0;
        
        /* 默认缓存配置 */
        cpu_cache[i].l1_size = 32 * 1024;       /* 32 KB */
        cpu_cache[i].l2_size = 256 * 1024;      /* 256 KB */
        cpu_cache[i].l3_size = 8 * 1024 * 1024; /* 8 MB */
        cpu_cache[i].l1_line_size = 64;
        cpu_cache[i].l2_line_size = 64;
        cpu_cache[i].cores_sharing_l3 = smp_cpu_count;
    }
    
    // println("[NUMA] Initialized (single node)");
}

/* 设置线程 CPU 亲和性 */
int sched_setaffinity(struct thread *thread, uint32_t cpu_mask) {
    if (thread == NULL) return -1;
    
    /* 验证 CPU 掩码有效 */
    uint32_t valid_mask = (1U << smp_cpu_count) - 1;
    if ((cpu_mask & valid_mask) == 0) return -1;
    
    thread->cpu_affinity = cpu_mask & valid_mask;
    
    /* 如果当前 CPU 不在亲和性掩码中，需要迁移 */
    if (thread->cpu >= 0 && !((1U << thread->cpu) & thread->cpu_affinity)) {
        /* 找一个亲和性掩码中的 CPU */
        for (int i = 0; i < smp_cpu_count; i++) {
            if ((1U << i) & thread->cpu_affinity) {
                migrate_thread(thread, i);
                break;
            }
        }
    }
    
    return 0;
}

/* 获取线程 CPU 亲和性 */
int sched_getaffinity(struct thread *thread, uint32_t *cpu_mask) {
    if (thread == NULL || cpu_mask == NULL) return -1;
    
    *cpu_mask = thread->cpu_affinity;
    return 0;
}

/* 选择最佳 CPU (考虑亲和性和缓存) */
int select_best_cpu(struct thread *thread) {
    if (thread == NULL) return 0;
    
    uint32_t affinity = thread->cpu_affinity;
    if (affinity == 0) affinity = (1U << smp_cpu_count) - 1;
    
    int best_cpu = -1;
    uint64_t best_score = (uint64_t)-1;
    uint64_t now = get_tick_count();
    
    /* 遍历亲和性掩码中的 CPU */
    for (int i = 0; i < smp_cpu_count; i++) {
        if (!((1U << i) & affinity)) continue;
        
        struct runqueue *rq = &runqueues[i];
        uint64_t score = 0;
        
        /* 负载因子 (越低越好) */
        score += rq->nr_total * 1000;
        
        /* 缓存亲和性 (上次在这运行加分) */
        if (thread->cpu == i) {
            score -= 500;  /* 缓存热度加成 */
        }
        
        /* NUMA 距离 */
        int thread_node = thread_get_numa_node(thread);
        int cpu_node = cpu_to_node[i];
        score += numa_nodes2[thread_node].distance[cpu_node] * 100;
        
        /* 空闲时间 (越久没运行越好) */
        if (thread->last_cpu == i && thread->affinity_history != NULL) {
            uint64_t idle_time = now - thread->affinity_history[i];
            score -= idle_time / 1000;
        }
        
        if (score < best_score) {
            best_score = score;
            best_cpu = i;
        }
    }
    
    return best_cpu >= 0 ? best_cpu : 0;
}

/* 缓存感知的负载均衡 */
void cache_aware_balance(void) {
    /* 检查是否需要缓存感知迁移 */
    for (int cpu = 0; cpu < smp_cpu_count; cpu++) {
        struct runqueue *rq = &runqueues[cpu];
        
        rq_lock(rq);
        
        /* 查找可能在其他 CPU 有更好缓存亲和性的线程 */
        for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
            struct list_head *head = &rq->queues[prio];
            struct thread *thread;
            
            list_for_each_entry(thread, head, run_list) {
                /* 如果线程最后运行的 CPU 不是当前 CPU */
                if (thread->last_cpu >= 0 && thread->last_cpu != cpu) {
                    /* 检查原 CPU 是否空闲 */
                    struct runqueue *orig_rq = &runqueues[thread->last_cpu];
                    
                    if (orig_rq->nr_total < rq->nr_total - 1) {
                        /* 迁移回原 CPU */
                        rq_unlock(rq);
                        migrate_thread(thread, thread->last_cpu);
                        rq_lock(rq);
                        break;
                    }
                }
            }
        }
        
        rq_unlock(rq);
    }
}

/* NUMA 感知的内存分配提示 */
int numa_get_preferred_node(struct thread *thread) {
    if (thread == NULL) return 0;
    
    /* 返回线程当前运行的 NUMA 节点 */
    return thread_get_numa_node(thread);
}

/* 设置 NUMA 内存策略 */
void numa_set_policy(struct thread *thread, int policy, int node) {
    if (thread == NULL) return;
    
    thread->numa_policy = policy;
    thread->numa_preferred_node = node;
}

/* 获取 NUMA 节点统计 */
void numa_node_stats(int node, uint64_t *total, uint64_t *free) {
    if (node < 0 || node >= numa_node_count) {
        *total = 0;
        *free = 0;
        return;
    }
    
    *total = numa_nodes2[node].memory_size;
    *free = numa_nodes2[node].free_memory;
}

/* 打印 NUMA 拓扑 */
void numa_print_topology(void) {
    // println("NUMA Topology:");
    for (int i = 0; i < numa_node_count; i++) {
        // print("  Node ");
        // print_dec(i);
        // print(": CPUs=");
        
        /* 打印 CPU 位掩码 */
        for (int j = 0; j < smp_cpu_count; j++) {
            if (numa_nodes2[i].cpu_mask & (1U << j)) {
                // print_dec(j);
                // print(" ");
            }
        }
        
        // print(" Memory=");
        // print_dec(numa_nodes2[i].memory_size / (1024 * 1024));
        // println(" MB");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：调度器节能模式
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 节能模式状态 */
static int power_save_mode = 0;
static uint64_t idle_threshold = 10000000ULL;  /* 10ms 空闲阈值 */

/* 启用节能模式 */
void scheduler_enable_power_save(int enable) {
    power_save_mode = enable;
}

/* 检查是否应该进入深睡眠 */
int should_deep_idle(int cpu) {
    if (!power_save_mode) return 0;
    
    struct runqueue *rq = &runqueues[cpu];
    if (rq->nr_total > 0) return 0;
    
    /* 检查全局负载 */
    for (int i = 0; i < smp_cpu_count; i++) {
        if (runqueues[i].nr_total > 0) return 0;
    }
    
    return 1;
}

/* 获取节能模式状态 */
int scheduler_get_power_save(void) {
    return power_save_mode;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：调度器实时诊断
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 检测调度器问题 */
struct sched_diagnosis {
    int has_starvation;         /* 是否有饥饿 */
    int has_load_imbalance;     /* 是否有负载不均 */
    int problem_cpu;            /* 问题 CPU */
    uint64_t max_latency;       /* 最大延迟 */
};

void scheduler_diagnose(struct sched_diagnosis *diag) {
    if (diag == NULL) return;
    
    memset(diag, 0, sizeof(*diag));
    
    uint64_t max_load = 0;
    uint64_t min_load = (uint64_t)-1;
    
    for (int i = 0; i < smp_cpu_count; i++) {
        struct runqueue *rq = &runqueues[i];
        
        if (rq->nr_total > max_load) {
            max_load = rq->nr_total;
            diag->problem_cpu = i;
        }
        if (rq->nr_total < min_load) {
            min_load = rq->nr_total;
        }
    }
    
    /* 负载不均检测 */
    if (max_load > min_load + 2 && min_load > 0) {
        diag->has_load_imbalance = 1;
    }
    
    /* 饥饿检测 */
    for (int i = 0; i < smp_cpu_count; i++) {
        struct runqueue *rq = &runqueues[i];
        
        rq_lock(rq);
        for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; prio++) {
            struct thread *thread;
            list_for_each_entry(thread, &rq->queues[prio], run_list) {
                uint64_t wait_time = get_tick_count() - thread->last_ran;
                if (wait_time > 1000000000ULL) {  /* 1秒 */
                    diag->has_starvation = 1;
                    break;
                }
            }
        }
        rq_unlock(rq);
    }
}
/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：组调度 (Group Scheduling)
 * 类似 Linux CFS 的组调度，公平分配 CPU 时间给进程组
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 调度组 */
struct sched_group {
    uint64_t shares;            /* 权重份额 */
    uint64_t load;              /* 当前负载 */
    uint64_t run_time;          /* 累计运行时间 */
    int nr_running;             /* 运行中的任务数 */
    struct list_head tasks;     /* 任务列表 */
    struct sched_group *parent;
    struct list_head children;
    struct list_head sibling;
    char name[32];
};

/* 调度组根 */
static struct sched_group root_group;

/* 初始化组调度 */
void sched_group_init(void) {
    root_group.shares = 1024;
    root_group.load = 0;
    root_group.run_time = 0;
    root_group.nr_running = 0;
    root_group.parent = NULL;
    INIT_LIST_HEAD(&root_group.tasks);
    INIT_LIST_HEAD(&root_group.children);
    strcpy(root_group.name, "root");
}

/* 创建调度组 */
struct sched_group *sched_group_create(const char *name, struct sched_group *parent) {
    struct sched_group *group = kmalloc(sizeof(struct sched_group));
    if (group == NULL) return NULL;
    
    group->shares = 1024;
    group->load = 0;
    group->run_time = 0;
    group->nr_running = 0;
    INIT_LIST_HEAD(&group->tasks);
    INIT_LIST_HEAD(&group->children);
    
    strncpy(group->name, name, sizeof(group->name) - 1);
    group->name[sizeof(group->name) - 1] = '\0';
    
    if (parent == NULL) {
        parent = &root_group;
    }
    group->parent = parent;
    list_add(&group->sibling, &parent->children);
    
    return group;
}

/* 设置组份额 */
void sched_group_set_shares(struct sched_group *group, uint64_t shares) {
    if (group == NULL || shares == 0) return;
    group->shares = shares;
}

/* 计算组权重 */
static uint64_t calc_group_weight(struct sched_group *group) {
    if (group == NULL) return 1024;
    
    /* 权重 = 份额 * (组内任务数 + 1) */
    return group->shares * (group->nr_running + 1);
}

/* 更新组负载 */
static void update_group_load(struct sched_group *group) {
    if (group == NULL) return;
    
    group->load = calc_group_weight(group);
    
    /* 递归更新父组 */
    if (group->parent != NULL) {
        update_group_load(group->parent);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：CPU 带宽控制 (CPU Bandwidth Control)
 * 限制进程组使用的 CPU 时间
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 带宽配额 */
struct cpu_bandwidth {
    uint64_t quota;             /* 配额 (纳秒)，-1 表示无限制 */
    uint64_t period;            /* 周期 (纳秒) */
    uint64_t runtime;           /* 当前周期内已使用时间 */
    uint64_t period_start;      /* 周期开始时间 */
    int throttled;              /* 是否被限流 */
};

static struct cpu_bandwidth bandwidth_control[MAX_CPUS];

/* 设置带宽限制 */
int sched_set_bandwidth(int cpu, uint64_t quota, uint64_t period) {
    if (cpu < 0 || cpu >= MAX_CPUS) return -1;
    
    struct cpu_bandwidth *bw = &bandwidth_control[cpu];
    bw->quota = quota;
    bw->period = period;
    bw->runtime = 0;
    bw->period_start = get_tick_count() * 1000;  /* 转换为纳秒 */
    bw->throttled = 0;
    
    return 0;
}

/* 检查带宽限制 */
int sched_check_bandwidth(int cpu, uint64_t time) {
    if (cpu < 0 || cpu >= MAX_CPUS) return 1;
    
    struct cpu_bandwidth *bw = &bandwidth_control[cpu];
    
    /* 无限制 */
    if (bw->quota == (uint64_t)-1) return 1;
    
    /* 检查是否需要开始新周期 */
    uint64_t now = get_tick_count() * 1000;
    if (now - bw->period_start >= bw->period) {
        bw->period_start = now;
        bw->runtime = 0;
        bw->throttled = 0;
    }
    
    /* 检查是否超过配额 */
    if (bw->runtime + time > bw->quota) {
        bw->throttled = 1;
        return 0;
    }
    
    return 1;
}

/* 记录带宽使用 */
void sched_account_bandwidth(int cpu, uint64_t time) {
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    
    struct cpu_bandwidth *bw = &bandwidth_control[cpu];
    
    if (bw->quota != (uint64_t)-1) {
        bw->runtime += time;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：调度器统计和监控
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 调度器统计 */
struct sched_statistics {
    uint64_t context_switches;
    uint64_t migrations;
    uint64_t wakeups;
    uint64_t missed_deadlines;
    uint64_t load_balance_count;
    uint64_t load_balance_failed;
    uint64_t preemptions;
    uint64_t voluntary_switches;
    uint64_t involuntary_switches;
};

static struct sched_statistics sched_stats[MAX_CPUS];

/* 更新统计 */
void sched_update_stats(int cpu, int event) {
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    
    struct sched_statistics *stats = &sched_stats[cpu];
    
    switch (event) {
        case 0: stats->context_switches++; break;
        case 1: stats->migrations++; break;
        case 2: stats->wakeups++; break;
        case 3: stats->missed_deadlines++; break;
        case 4: stats->load_balance_count++; break;
        case 5: stats->load_balance_failed++; break;
        case 6: stats->preemptions++; break;
        case 7: stats->voluntary_switches++; break;
        case 8: stats->involuntary_switches++; break;
    }
}

/* 获取调度器统计 */
void sched_get_stats(int cpu, struct sched_statistics *stats) {
    if (cpu < 0 || cpu >= MAX_CPUS || stats == NULL) return;
    *stats = sched_stats[cpu];
}

/* 打印调度器统计 */
void sched_print_stats(void) {
    // println("=== Scheduler Statistics ===");
    
    struct sched_statistics total;
    memset(&total, 0, sizeof(total));
    
    for (int i = 0; i < smp_cpu_count; i++) {
        total.context_switches += sched_stats[i].context_switches;
        total.migrations += sched_stats[i].migrations;
        total.wakeups += sched_stats[i].wakeups;
        total.missed_deadlines += sched_stats[i].missed_deadlines;
        total.load_balance_count += sched_stats[i].load_balance_count;
        total.preemptions += sched_stats[i].preemptions;
    }
    
    // print("Context switches: ");
    // print_dec(total.context_switches);
    // println("");
    
    // print("Migrations: ");
    // print_dec(total.migrations);
    // println("");
    
    // print("Wakeups: ");
    // print_dec(total.wakeups);
    // println("");
    
    // print("Load balances: ");
    // print_dec(total.load_balance_count);
    // println("");
    
    // print("Preemptions: ");
    // print_dec(total.preemptions);
    // println("");
    
    // print("Missed deadlines: ");
    // print_dec(total.missed_deadlines);
    // println("");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：自动 NUMA 放置 (Automatic NUMA Placement)
 * 根据内存访问模式自动优化任务放置
 * ═══════════════════════════════════════════════════════════════════════════ */

/* NUMA 访问统计 */
struct numa_access_stats {
    uint64_t local_accesses;
    uint64_t remote_accesses;
    int preferred_node;
    uint64_t last_scan;
};

/* 更新 NUMA 访问统计 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：调度器自调优 (Self-tuning Scheduler)
 * 根据工作负载自动调整调度参数
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 调度器参数 */
struct sched_params {
    int min_granularity;        /* 最小调度粒度 (us) */
    int wakeup_granularity;     /* 唤醒粒度 (us) */
    int latency;                /* 调度延迟 (us) */
    int migration_cost;         /* 迁移代价 (us) */
};

static struct sched_params sched_tuning = {
    .min_granularity = 1000,    /* 1ms */
    .wakeup_granularity = 1000,
    .latency = 6000,            /* 6ms */
    .migration_cost = 5000      /* 5ms */
};

/* 自调优 - 根据负载调整参数 */
void sched_auto_tune(void) {
    uint64_t total_load = 0;
    
    for (int i = 0; i < smp_cpu_count; i++) {
        total_load += runqueues[i].nr_total;
    }
    
    /* 高负载时减少粒度，提高响应性 */
    if (total_load > smp_cpu_count * 10) {
        sched_tuning.min_granularity = 500;  /* 0.5ms */
        sched_tuning.latency = 3000;         /* 3ms */
    } else if (total_load < smp_cpu_count * 2) {
        /* 低负载时增加粒度，减少开销 */
        sched_tuning.min_granularity = 2000; /* 2ms */
        sched_tuning.latency = 10000;        /* 10ms */
    } else {
        /* 默认值 */
        sched_tuning.min_granularity = 1000;
        sched_tuning.latency = 6000;
    }
}

/* 获取调度参数 */
void sched_get_params(struct sched_params *params) {
    if (params != NULL) {
        *params = sched_tuning;
    }
}

/* 设置调度参数 */
void sched_set_params(struct sched_params *params) {
    if (params != NULL) {
        sched_tuning = *params;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：优先级提升 (Priority Boosting)
 * 防止低优先级任务饥饿
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 优先级提升配置 */
#define BOOST_INTERVAL     10000000ULL  /* 10ms */
#define BOOST_DURATION     1000000ULL   /* 1ms */

static uint64_t last_boost_time = 0;
static int boost_active = 0;

/* 检查是否需要优先级提升 */
int sched_check_boost(uint64_t wait_time) {
    uint64_t now = get_tick_count();
    
    /* 定期激活提升 */
    if (now - last_boost_time > BOOST_INTERVAL) {
        last_boost_time = now;
        boost_active = 1;
    }
    
    /* 提升期间，等待超过阈值的任务获得提升 */
    if (boost_active && wait_time > BOOST_DURATION) {
        return 1;
    }
    
    /* 检查是否结束提升周期 */
    if (boost_active && now - last_boost_time > BOOST_DURATION) {
        boost_active = 0;
    }
    
    return 0;
}

/* 应用优先级提升 */



/* ========== 调度器高级增强功能 ========== */



/* === 调度器诊断 === */



struct sched_diag_info {

    uint64_t migrations;

    uint64_t load_balances;

    uint64_t preemptions;

    uint64_t wakeups;

    uint64_t context_switches;

    uint64_t avg_latency;

    uint64_t max_latency;

};



static struct sched_diag_info sched_diag[MAX_CPUS];



/* 记录调度事件 */

void sched_record_diag(int cpu, int event, uint64_t val) {

    if (cpu < 0 || cpu >= MAX_CPUS) return;

    

    switch (event) {

        case 0: sched_diag[cpu].migrations++; break;

        case 1: sched_diag[cpu].load_balances++; break;

        case 2: sched_diag[cpu].preemptions++; break;

        case 3: sched_diag[cpu].wakeups++; break;

        case 4: sched_diag[cpu].context_switches++; break;

        case 5:

            sched_diag[cpu].avg_latency = (sched_diag[cpu].avg_latency * 7 + val) / 8;

            if (val > sched_diag[cpu].max_latency) sched_diag[cpu].max_latency = val;

            break;

    }

}



/* 打印调度器诊断 */

void sched_print_diag(void) {

    // println("=== Scheduler Diagnostics ===");

    for (uint32_t i = 0; i < smp_cpu_count; i++) {

        // print("CPU "); print_dec(i); println(":");

        // print("  Migrations: "); print_dec(sched_diag[i].migrations); println("");

        // print("  Load Balances: "); print_dec(sched_diag[i].load_balances); println("");

        // print("  Context Switches: "); print_dec(sched_diag[i].context_switches); println("");

    }

}



/* === 能耗感知调度 === */



struct cpu_energy_model {

    uint64_t capacity;

    uint64_t max_freq;

    uint64_t idle_power;

    uint64_t max_power;

};



static struct cpu_energy_model cpu_energy[MAX_CPUS];



/* 初始化能耗模型 */

void sched_energy_init(void) {

    for (int i = 0; i < MAX_CPUS; i++) {

        cpu_energy[i].capacity = 1024;

        cpu_energy[i].max_freq = 3000000;

        cpu_energy[i].idle_power = 10;

        cpu_energy[i].max_power = 5000;

    }

}



/* 计算能耗最优 CPU */

int sched_find_energy_cpu(void) {

    int best = -1;

    uint64_t best_energy = ~0ULL;

    

    for (uint32_t i = 0; i < smp_cpu_count; i++) {

        struct cpu_energy_model *em = &cpu_energy[i];

        struct runqueue *rq = &runqueues[i];

        

        uint64_t spare = em->capacity - rq->nr_total * 100;

        uint64_t energy = em->idle_power + 

            (em->max_power - em->idle_power) * (em->capacity - spare) / em->capacity;

        

        if (spare >= 512 && energy < best_energy) {

            best_energy = energy;

            best = i;

        }

    }

    

    return best >= 0 ? best : 0;

}



/* === CPU 热插拔支持 === */



int sched_cpu_online_handler(int cpu) {

    if (cpu < 0 || cpu >= MAX_CPUS) return -1;

    

    runqueues[cpu].nr_total = 0;

    runqueues[cpu].curr = NULL;

    rt_rq_init(&rt_rqs[cpu]);

    

    return 0;

}



int sched_cpu_offline_handler(int cpu) {

    if (cpu < 0 || cpu >= MAX_CPUS) return -1;

    

    struct runqueue *rq = &runqueues[cpu];

    

    while (rq->nr_total > 0) {

        int target = (cpu + 1) % smp_cpu_count;

        if (target == cpu) break;

        

        rq->nr_total--;

        runqueues[target].nr_total++;

    }

    

    return 0;

}



/* === 调度器压力测试 === */



void sched_stress_test(int iter) {

    // print("[SCHED] Stress test: ");

    // print_dec(iter);

    // println(" iterations");

    

    for (int i = 0; i < iter; i++) {

        for (uint32_t c = 0; c < smp_cpu_count; c++) {

            load_balance(c);

        }

        sched_auto_tune();

    }

    

    // println("[SCHED] Stress test complete");

    sched_print_diag();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * EEVDF (Earliest Eligible Virtual Deadline First) 调度器
 *
 * Linux 6.6+ 引入的下一代调度算法，替代 CFS
 * 核心思想:
 *   - 每个任务有虚拟截止时间 (virtual deadline)
 *   - 调度器选择最早合格的虚拟截止时间任务
 *   - 合格性基于公平份额计算 (lag-based eligibility)
 *   - 相比 CFS，减少延迟敏感任务的调度延迟
 *   - 更好的响应性和公平性权衡
 *
 * 关键公式:
 *   vdeadline = vruntime + slice * weight / total_weight
 *   eligible  = vruntime <= avg_vruntime + slice
 *   lag       = (avg_vruntime - vruntime) * weight
 * ═══════════════════════════════════════════════════════════════════════════ */

/* EEVDF 配置 */
#define EEVDF_BASE_SLICE_NS     3000000ULL   /* 基础时间片 3ms */
#define EEVDF_MIN_SLICE_NS     300000ULL    /* 最小时间片 0.3ms */
#define EEVDF_MAX_SLICE_NS     100000000ULL /* 最大时间片 100ms */
#define EEVDF_SCHED_PERIOD_NS  6000000ULL   /* 调度周期 6ms */
#define EEVDF_MIN_GRAN_NS      750000ULL    /* 最小粒度 0.75ms */
#define EEVDF_WAKEUP_GRAN_NS   1000000ULL   /* 唤醒抢占粒度 1ms */

/* EEVDF 调度实体 (扩展 sched_entity) */
struct eevdf_entity {
    struct rb_node run_node;        /* 红黑树节点 */
    struct thread *thread;          /* 所属线程 */

    /* 虚拟时间 */
    uint64_t vruntime;              /* 虚拟运行时间 */
    uint64_t vdeadline;             /* 虚拟截止时间 */

    /* 权重 */
    uint32_t weight;                /* 调度权重 */
    uint32_t wmult_idx;             /* 逆权重表索引 */
    int nice;                       /* nice 值 */

    /* 合格性 */
    int eligible;                   /* 是否合格可被调度 */

    /* 时间片 */
    uint64_t slice;                 /* 当前时间片 */
    uint64_t sum_exec_runtime;      /* 累计执行时间 */
    uint64_t prev_sum_exec_runtime; /* 上次累计执行时间 */

    /* 状态 */
    int on_rq;                      /* 是否在运行队列中 */

    /* 延迟敏感度 */
    int latency_sensitive;          /* 延迟敏感标志 */
    uint64_t avg_runtime;           /* 平均运行时间 (用于自适应) */
};

/* EEVDF 运行队列 (Per-CPU) */
struct eevdf_rq {
    /* 红黑树 (按 vdeadline 排序) */
    struct rb_root timeline;        /* 主时间线 */
    struct rb_root eligible_tree;   /* 合格任务树 */

    /* 当前运行实体 */
    struct eevdf_entity *curr;

    /* 统计 */
    uint32_t nr_running;
    uint64_t load_weight;           /* 总负载权重 */
    uint64_t runnable_weight;

    /* 虚拟时间 */
    uint64_t min_vruntime;          /* 最小虚拟时间 */
    uint64_t avg_vruntime;          /* 平均虚拟时间 */
    uint64_t avg_load;              /* 平均负载 */

    /* 时间 */
    uint64_t clock;                 /* 队列时钟 */

    /* 锁 */
    volatile int lock;

    /* CPU */
    int cpu;
} __attribute__((aligned(64)));

/* Per-CPU EEVDF 运行队列 */
static struct eevdf_rq eevdf_rqs[MAX_CPUS];

/* EEVDF 获取当前时间 (纳秒) */
static uint64_t eevdf_get_time(void) {
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    return tsc * 1000000000ULL / 3000000000ULL;
}

/* 初始化 EEVDF 运行队列 */
static void eevdf_rq_init(struct eevdf_rq *rq, int cpu) {
    rq->timeline.rb_node = NULL;
    rq->timeline.rb_leftmost = NULL;
    rq->timeline.nr_nodes = 0;
    rq->eligible_tree.rb_node = NULL;
    rq->eligible_tree.rb_leftmost = NULL;
    rq->eligible_tree.nr_nodes = 0;

    rq->curr = NULL;
    rq->nr_running = 0;
    rq->load_weight = 0;
    rq->runnable_weight = 0;
    rq->min_vruntime = 0;
    rq->avg_vruntime = 0;
    rq->avg_load = 0;
    rq->clock = 0;
    rq->lock = 0;
    rq->cpu = cpu;
}

/* 初始化 EEVDF 实体 */
static void eevdf_init_entity(struct eevdf_entity *ee, struct thread *thread) {
    ee->thread = thread;
    ee->vruntime = 0;
    ee->vdeadline = 0;
    ee->weight = 1024;  /* NICE_0_LOAD */
    ee->wmult_idx = 20; /* nice=0 */
    ee->nice = 0;
    ee->eligible = 1;
    ee->slice = EEVDF_BASE_SLICE_NS;
    ee->sum_exec_runtime = 0;
    ee->prev_sum_exec_runtime = 0;
    ee->on_rq = 0;
    ee->latency_sensitive = 0;
    ee->avg_runtime = 0;
}

/* EEVDF 自适应调度周期 */
static uint64_t eevdf_sched_period(struct eevdf_rq *rq) {
    uint64_t period = EEVDF_SCHED_PERIOD_NS;
    if (rq->nr_running > 0) {
        if (period / rq->nr_running < EEVDF_MIN_GRAN_NS)
            period = (uint64_t)rq->nr_running * EEVDF_MIN_GRAN_NS;
    }
    return period;
}

/* 计算时间片 */
static uint64_t eevdf_calc_slice(struct eevdf_rq *rq, struct eevdf_entity *ee) {
    if (rq->nr_running == 0) return EEVDF_BASE_SLICE_NS;

    uint64_t period = eevdf_sched_period(rq);
    uint64_t slice = period * ee->weight;

    /* 使用逆权重乘法替代除法 */
    if (rq->load_weight > 0) {
        slice = slice / rq->load_weight;
    }

    /* 延迟敏感任务获得更短时间片但更频繁调度 */
    if (ee->latency_sensitive) {
        slice = slice * 3 / 4;  /* 缩短 25% */
    }

    if (slice < EEVDF_MIN_SLICE_NS) slice = EEVDF_MIN_SLICE_NS;
    if (slice > EEVDF_MAX_SLICE_NS) slice = EEVDF_MAX_SLICE_NS;

    return slice;
}

/* 计算虚拟截止时间 */
static void eevdf_update_deadline(struct eevdf_rq *rq, struct eevdf_entity *ee) {
    ee->slice = eevdf_calc_slice(rq, ee);
    ee->vdeadline = ee->vruntime + ee->slice;
}

/* 更新平均虚拟时间 */
static void eevdf_update_avg_vruntime(struct eevdf_rq *rq) {
    if (rq->nr_running == 0) {
        rq->avg_vruntime = rq->min_vruntime;
        return;
    }

    /* avg_vruntime = sum(vruntime * weight) / total_weight */
    /* 简化：使用 min_vruntime 作为近似 */
    if (rq->curr) {
        rq->avg_vruntime = rq->curr->vruntime;
    }
}

/* 检查合格性 */
static int eevdf_entity_eligible(struct eevdf_rq *rq, struct eevdf_entity *ee) {
    /* 合格条件: vruntime <= avg_vruntime + slice */
    /* 简化: 如果实体的虚拟时间不超过平均值太多，则合格 */
    if (rq->nr_running <= 1) return 1;

    /* lag = (avg_vruntime - vruntime) * weight */
    /* 如果 lag >= 0，说明该实体欠了 CPU 时间，应该被调度 */
    int64_t lag = (int64_t)(rq->avg_vruntime - ee->vruntime);
    return lag >= 0 || (uint64_t)(-lag) < ee->slice;
}

/* 更新虚拟运行时间 */
static void eevdf_update_vruntime(struct eevdf_rq *rq, struct eevdf_entity *ee, uint64_t delta) {
    if (!ee) return;

    /* 使用逆权重乘法计算虚拟时间增量 */
    uint32_t wmult = sched_prio_to_wmult[ee->wmult_idx];
    uint64_t vruntime_delta = (delta * wmult) >> 32;

    ee->sum_exec_runtime += delta;
    ee->vruntime += vruntime_delta;

    /* 更新平均运行时间 (指数移动平均) */
    ee->avg_runtime = (ee->avg_runtime * 7 + delta) / 8;

    /* 自适应延迟敏感度 */
    if (ee->avg_runtime < EEVDF_BASE_SLICE_NS && ee->avg_runtime > 0) {
        ee->latency_sensitive = 1;
    } else {
        ee->latency_sensitive = 0;
    }

    /* 更新截止时间 */
    eevdf_update_deadline(rq, ee);

    /* 保守更新 min_vruntime */
    if (ee->vruntime > rq->min_vruntime) {
        rq->min_vruntime = ee->vruntime;
    }
}

/* EEVDF 入队 */
static void eevdf_enqueue(struct eevdf_rq *rq, struct eevdf_entity *ee) {
    if (!ee || ee->on_rq) return;

    while (__sync_lock_test_and_set(&rq->lock, 1))
        asm volatile("pause");

    /* 初始化虚拟时间 */
    if (ee->vruntime == 0) {
        ee->vruntime = rq->min_vruntime;
    }

    /* 更新合格性 */
    ee->eligible = eevdf_entity_eligible(rq, ee);

    /* 设置红黑树键值为 vdeadline */
    ee->run_node.key = ee->vdeadline;

    /* 插入时间线 */
    rb_insert(&rq->timeline, &ee->run_node);

    ee->on_rq = 1;
    rq->nr_running++;
    rq->load_weight += ee->weight;
    rq->runnable_weight += ee->weight;

    /* 更新截止时间 */
    eevdf_update_deadline(rq, ee);

    /* 更新平均虚拟时间 */
    eevdf_update_avg_vruntime(rq);

    __sync_lock_release(&rq->lock);
}

/* EEVDF 出队 */
static void eevdf_dequeue(struct eevdf_rq *rq, struct eevdf_entity *ee) {
    if (!ee || !ee->on_rq) return;

    while (__sync_lock_test_and_set(&rq->lock, 1))
        asm volatile("pause");

    rb_erase(&rq->timeline, &ee->run_node);

    ee->on_rq = 0;
    rq->nr_running--;
    rq->load_weight -= ee->weight;
    rq->runnable_weight -= ee->weight;

    if (rq->curr == ee) {
        rq->curr = NULL;
    }

    eevdf_update_avg_vruntime(rq);

    __sync_lock_release(&rq->lock);
}

/* EEVDF 选择下一个任务 - 核心算法 */
static struct eevdf_entity *eevdf_pick_next(struct eevdf_rq *rq) {
    struct rb_node *node = rb_first(&rq->timeline);
    if (!node) return NULL;

    struct eevdf_entity *best = container_of(node, struct eevdf_entity, run_node);

    /* EEVDF 核心：选择最早合格的虚拟截止时间任务 */
    /* 遍历树找到第一个合格的任务 */
    while (node) {
        struct eevdf_entity *ee = container_of(node, struct eevdf_entity, run_node);

        /* 更新合格性 */
        ee->eligible = eevdf_entity_eligible(rq, ee);

        if (ee->eligible) {
            /* 找到合格任务，比较截止时间 */
            if (ee->vdeadline < best->vdeadline || !best->eligible) {
                best = ee;
            }
            /* 合格任务中截止时间最早的，无需继续搜索 */
            break;
        }

        node = rb_next(node);
    }

    /* 如果没有合格任务，选择虚拟时间最小的 (最欠 CPU 时间的) */
    if (!best->eligible) {
        node = rb_first(&rq->timeline);
        if (node) {
            best = container_of(node, struct eevdf_entity, run_node);
        }
    }

    return best;
}

/* EEVDF 时钟 tick */
static void eevdf_task_tick(struct eevdf_rq *rq, struct eevdf_entity *ee) {
    if (!ee) return;

    uint64_t now = eevdf_get_time();
    uint64_t delta = now - rq->clock;
    rq->clock = now;

    if (delta > 0 && ee == rq->curr) {
        eevdf_update_vruntime(rq, ee, delta);
    }

    /* 检查时间片是否用完 */
    if (ee->sum_exec_runtime - ee->prev_sum_exec_runtime >= ee->slice) {
        if (ee->thread) {
            ee->thread->flags |= THREAD_FLAG_NEED_RESCHED;
        }
        ee->prev_sum_exec_runtime = ee->sum_exec_runtime;
    }

    /* 检查是否有更早截止时间的合格任务 */
    struct rb_node *leftmost = rb_first(&rq->timeline);
    if (leftmost) {
        struct eevdf_entity *next = container_of(leftmost, struct eevdf_entity, run_node);
        next->eligible = eevdf_entity_eligible(rq, next);

        if (next->eligible && next->vdeadline < ee->vdeadline) {
            /* 新任务的截止时间更早，触发抢占 */
            if (ee->vdeadline - next->vdeadline > EEVDF_WAKEUP_GRAN_NS) {
                if (ee->thread) {
                    ee->thread->flags |= THREAD_FLAG_NEED_RESCHED;
                }
            }
        }
    }
}

/* EEVDF 唤醒抢占检查 */
static int eevdf_check_preempt_wakeup(struct eevdf_rq *rq,
                                       struct eevdf_entity *curr,
                                       struct eevdf_entity *se) {
    if (!curr || !se) return 0;

    /* 延迟敏感任务优先 */
    if (se->latency_sensitive && !curr->latency_sensitive) {
        return 1;
    }

    /* 如果唤醒任务的截止时间更早，可以抢占 */
    if (se->vdeadline < curr->vdeadline) {
        return 1;
    }

    /* 如果唤醒任务欠 CPU 时间 (lag > 0)，可以抢占 */
    int64_t lag = (int64_t)(rq->avg_vruntime - se->vruntime);
    if (lag > 0 && (uint64_t)lag > EEVDF_WAKEUP_GRAN_NS) {
        return 1;
    }

    return 0;
}

/* EEVDF 放回当前任务 */
static void eevdf_put_prev_task(struct eevdf_rq *rq, struct eevdf_entity *ee) {
    if (!ee) return;

    uint64_t now = eevdf_get_time();
    uint64_t delta = now - rq->clock;
    if (delta > 0) {
        eevdf_update_vruntime(rq, ee, delta);
    }

    rq->curr = NULL;

    /* 重新入队 */
    if (ee->on_rq) {
        rb_erase(&rq->timeline, &ee->run_node);
        ee->run_node.key = ee->vdeadline;
        rb_insert(&rq->timeline, &ee->run_node);
    }
}

/* EEVDF 设置 nice 值 */
static void eevdf_set_nice(struct eevdf_entity *ee, int nice) {
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;

    ee->nice = nice;
    ee->weight = sched_prio_to_weight[nice + 20];
    ee->wmult_idx = (uint32_t)(nice + 20);
}

/* EEVDF 负载均衡 - 使用安全的双锁获取避免 ABBA 死锁 */
static void eevdf_load_balance(struct eevdf_rq *src, struct eevdf_rq *dst) {
    if (!src || !dst) return;
    if (src->nr_running <= dst->nr_running + 1) return;

    /* 按地址排序获取锁，避免死锁 */
    struct eevdf_rq *first = (src < dst) ? src : dst;
    struct eevdf_rq *second = (src < dst) ? dst : src;

    /* 使用 trylock + fallback 策略：先尝试获取第一把锁，
     * 再 trylock 第二把，失败则释放重试 */
    int retries = 0;
    while (retries < 100) {
        while (__sync_lock_test_and_set(&first->lock, 1))
            asm volatile("pause");

        if (__sync_lock_test_and_set(&second->lock, 1) == 0) {
            /* 成功获取两把锁 */
            break;
        }

        /* 第二把锁获取失败，释放第一把锁并短暂等待后重试 */
        __sync_lock_release(&first->lock);
        for (volatile int i = 0; i < (1 << (retries < 5 ? retries : 5)); i++)
            asm volatile("pause");
        retries++;
    }

    if (retries >= 100) {
        /* 超时放弃，避免死锁 */
        return;
    }

    struct rb_node *node = rb_first(&src->timeline);
    if (node) {
        struct eevdf_entity *ee = container_of(node, struct eevdf_entity, run_node);

        rb_erase(&src->timeline, node);
        src->nr_running--;
        src->load_weight -= ee->weight;

        /* 调整虚拟时间 */
        if (ee->vruntime < dst->min_vruntime) {
            ee->vruntime = dst->min_vruntime;
        }

        ee->run_node.key = ee->vdeadline;
        rb_insert(&dst->timeline, &ee->run_node);
        dst->nr_running++;
        dst->load_weight += ee->weight;
    }

    /* 按相反顺序释放锁 */
    __sync_lock_release(&second->lock);
    __sync_lock_release(&first->lock);
}

/* 初始化 EEVDF 调度器 */
void eevdf_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        eevdf_rq_init(&eevdf_rqs[i], i);
    }
    serial_print("[EEVDF] Earliest Eligible Virtual Deadline First scheduler initialized\n");
}

/* 获取当前 CPU 的 EEVDF 运行队列 */
static inline struct eevdf_rq *this_eevdf_rq(void) {
    return &eevdf_rqs[smp_processor_id()];
}


