/*
 * Zratro OS - Scheduler Header
 * 
 * 支持 Per-CPU 运行队列的多核调度器
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "task.h"
#include "idt.h"
#include "smp.h"

/* 调度器优先级队列数量 */
#define SCHED_PRIORITY_LEVELS   7

/* 时间片配置 */
#define DEFAULT_TIME_SLICE      10      /* 默认时间片 (ticks) */
#define MIN_TIME_SLICE          1
#define MAX_TIME_SLICE          100

/* 调度器标志 */
#define SCHED_FLAG_NEED_RESCHED   (1 << 0)
#define SCHED_FLAG_NEED_LOAD_BAL  (1 << 1)

/* ========== 等待队列 (前向定义) ========== */

/* 等待队列 */
struct wait_queue {
    struct thread *thread;
    struct list_head list;
};

/* 等待队列头 */
struct wait_queue_head {
    volatile int lock;
    struct list_head list;
};

/* 等待队列头类型别名 */
typedef struct wait_queue_head wait_queue_head_t;

/* 运行队列结构 (Per-CPU) */
struct runqueue {
    /* 优先级队列 (每个优先级一个链表) */
    struct list_head queues[SCHED_PRIORITY_LEVELS];
    uint32_t nr_running[SCHED_PRIORITY_LEVELS];  /* 每个优先级的线程数 */
    
    /* 红黑树时间线 (按 vruntime 排序，O(log n) 查找最小值) */
    struct rb_root tasks_timeline;       /* 红黑树根 */
    struct rb_node *rb_leftmost;         /* 最左节点缓存，O(1) 取最小 */
    
    /* 统计 */
    uint32_t nr_total;        /* 总运行线程数 */
    uint64_t load_weight;     /* 负载权重 */
    uint64_t last_balance;    /* 上次负载均衡时间 */
    
    /* 当前运行的线程 */
    struct thread *curr;
    
    /* 锁 */
    volatile int lock;
} __attribute__((aligned(64)));

/* 调度类 */
struct sched_class {
    const char *name;
    
    /* 入队 */
    void (*enqueue)(struct runqueue *rq, struct thread *thread);
    
    /* 出队 */
    void (*dequeue)(struct runqueue *rq, struct thread *thread);
    
    /* 选择下一个 */
    struct thread *(*pick_next)(struct runqueue *rq);
    
    /* 时间片用完 */
    void (*tick)(struct runqueue *rq, struct thread *thread);
    
    /* 负载均衡 */
    void (*balance)(struct runqueue *rq, int cpu);
};

/* ========== 初始化 ========== */

/* 初始化调度器 */
void scheduler_init(void);

/* 初始化 Per-CPU 运行队列 */
void scheduler_init_cpu(int cpu_id);

/* 启动调度器 */
void scheduler_start(void);

/* ========== 线程操作 ========== */

/* 添加线程到调度队列 */
void scheduler_add_thread(struct thread *thread);

/* 从调度队列移除线程 */
void scheduler_remove_thread(struct thread *thread);

/* 选择下一个线程 */
struct thread *scheduler_pick_next(void);

/* 触发调度 */
void schedule(void);

/* 主动让出 CPU */
void yield(void);

/* 线程睡眠 */
void sleep_on(struct wait_queue_head *wq);

/* 唤醒线程 */
void wake_up(struct wait_queue_head *wq);
void wake_up_process(struct thread *thread);

/* ========== 定时器 ========== */

/* 调度时钟中断 */
void scheduler_tick(struct interrupt_frame *frame);

/* ========== 负载均衡 ========== */

/* 触发负载均衡 */
void trigger_load_balance(int cpu);

/* 执行负载均衡 */
void load_balance(int cpu);

/* 迁移线程 */
int migrate_thread(struct thread *thread, int target_cpu);

/* ========== 优先级操作 ========== */

/* 设置线程优先级 */
void set_thread_priority(struct thread *thread, int priority);

/* 获取线程优先级 */
static inline int get_thread_priority(struct thread *thread) {
    return thread->priority;
}

/* ========== 运行队列访问 ========== */

/* 获取当前 CPU 的运行队列 */
static inline struct runqueue *this_rq(void) {
    extern struct runqueue runqueues[];
    return &runqueues[smp_processor_id()];
}

/* 获取指定 CPU 的运行队列 */
static inline struct runqueue *cpu_rq(int cpu) {
    extern struct runqueue runqueues[];
    return &runqueues[cpu];
}

/* 运行队列锁 */
static inline void rq_lock(struct runqueue *rq) {
    while (__sync_lock_test_and_set(&rq->lock, 1)) {
        asm volatile("pause");
    }
}

static inline void rq_unlock(struct runqueue *rq) {
    __sync_lock_release(&rq->lock);
}

/* ========== 调度标志 ========== */

/* 设置需要调度标志 */
static inline void set_need_resched(void) {
    struct cpu_local *cpu = this_cpu();
    cpu->current_thread->flags |= THREAD_FLAG_NEED_RESCHED;
}

/* 清除需要调度标志 */
static inline void clear_need_resched(void) {
    struct cpu_local *cpu = this_cpu();
    if (cpu->current_thread) {
        cpu->current_thread->flags &= ~THREAD_FLAG_NEED_RESCHED;
    }
}

/* 检查是否需要调度 */
static inline int need_resched(void) {
    struct cpu_local *cpu = this_cpu();
    return cpu->current_thread && 
           (cpu->current_thread->flags & THREAD_FLAG_NEED_RESCHED);
}

/* ========== 统计 ========== */

/* 打印调度器统计信息 */
void scheduler_stats(void);

/* 打印所有 CPU 运行队列状态 */
void scheduler_dump_all(void);

/* ========== 空闲任务 ========== */

/* 空闲任务函数 */
void idle_task(void *arg);

/* ========== 等待队列操作 ========== */

/* 初始化等待队列 */
static inline void init_waitqueue_head(wait_queue_head_t *wq) {
    wq->lock = 0;
    INIT_LIST_HEAD(&wq->list);
}

/* 定义等待队列 */
#define DEFINE_WAIT_QUEUE(name) \
    wait_queue_head_t name = { .lock = 0, .list = { &name.list, &name.list } }

#endif /* SCHEDULER_H */