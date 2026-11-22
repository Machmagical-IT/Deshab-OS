/*
 * Zratro OS - RCU (Read-Copy-Update) Implementation
 * 
 * 高性能读端无锁同步机制
 */

#include "rcu.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"
#include "apic.h"
#include "task.h"

/* ========== 全局变量 ========== */

/* RCU 状态 */
static struct rcu_state rcu_state;
static struct rcu_data rcu_data[MAX_CPUS];
static struct rcu_node rcu_nodes[256];  /* 节点池 */

/* 调试开关 */
int debug_rcu_head_enabled = 0;

/* 回调批处理 */
static struct list_head rcu_cb_batch;
static uint64_t rcu_cb_batch_size;

/* 宽限期状态 */
#define RCU_GP_IDLE        0
#define RCU_GP_WAIT        1
#define RCU_GP_IN_PROGRESS 2

/* ========== 初始化 ========== */

int rcu_init(void) {
    println("[RCU] Initializing RCU subsystem...");
    
    /* 初始化 RCU 状态 */
    memset(&rcu_state, 0, sizeof(rcu_state));
    rcu_state.gp_seq = 0;
    rcu_state.gp_state = RCU_GP_IDLE;
    rcu_state.lock = SPINLOCK_INIT;
    list_init(&rcu_state.cblist);
    
    /* 初始化 Per-CPU 数据 */
    for (int i = 0; i < MAX_CPUS; i++) {
        struct rcu_data *rdp = &rcu_data[i];
        memset(rdp, 0, sizeof(*rdp));
        rdp->cpu = i;
        rdp->gp_seq = 0;
        list_init(&rdp->cblist);
        list_init(&rdp->done_cbs);
    }
    
    /* 初始化节点池 */
    for (int i = 0; i < 256; i++) {
        rcu_nodes[i].lock = SPINLOCK_INIT;
    }
    
    /* 初始化回调批处理 */
    list_init(&rcu_cb_batch);
    rcu_cb_batch_size = 0;
    
    println("[RCU] RCU initialized successfully");
    return 0;
}

/* ========== 宽限期管理 ========== */

/* 等待所有 CPU 通过静默状态 */
static int rcu_implicit_cpu_qs_all(void) {
    /* 检查所有 CPU 是否都通过了静默状态 */
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        struct rcu_data *rdp = &rcu_data[i];
        
        /* 如果 CPU 在 RCU 临界区，还未通过 QS */
        if (rdp->cpu_no_qs && this_cpu()->rcu_nesting > 0) {
            return 0;
        }
    }
    
    return 1;
}

/* 报告 CPU 静默状态 */
void rcu_report_qs_rdp(int cpu, struct rcu_data *rdp) {
    if (rdp == NULL) return;
    
    spin_lock(&rcu_state.lock);
    
    rdp->cpu_no_qs = 0;
    rdp->core_needs_qs = 0;
    
    /* 检查是否所有 CPU 都通过了 QS */
    if (rcu_implicit_cpu_qs_all()) {
        /* 宽限期完成 */
        rcu_state.gp_seq++;
        rcu_state.gp_state = RCU_GP_IDLE;
    }
    
    spin_unlock(&rcu_state.lock);
}

/* 启动新宽限期 */
static void rcu_start_gp(void) {
    spin_lock(&rcu_state.lock);
    
    if (rcu_state.gp_state != RCU_GP_IDLE) {
        spin_unlock(&rcu_state.lock);
        return;
    }
    
    rcu_state.gp_state = RCU_GP_IN_PROGRESS;
    rcu_state.gp_start_time = get_tick_count();  /* 记录宽限期开始时间 */
    
    /* 重置所有 CPU 的 QS 状态 */
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        rcu_data[i].cpu_no_qs = 1;
        rcu_data[i].core_needs_qs = 1;
    }
    
    spin_unlock(&rcu_state.lock);
}

/* 同步等待宽限期 */
void synchronize_rcu(void) {
    uint64_t start_gp_seq;
    
    /* 记录当前宽限期 */
    spin_lock(&rcu_state.lock);
    start_gp_seq = rcu_state.gp_seq;
    spin_unlock(&rcu_state.lock);
    
    /* 等待宽限期完成 */
    while (1) {
        /* 检查宽限期是否已前进 */
        spin_lock(&rcu_state.lock);
        uint64_t cur_gp_seq = rcu_state.gp_seq;
        int gp_state = rcu_state.gp_state;
        spin_unlock(&rcu_state.lock);
        
        if (cur_gp_seq > start_gp_seq && gp_state == RCU_GP_IDLE) {
            break;
        }
        
        /* 如果宽限期未启动，启动一个 */
        if (gp_state == RCU_GP_IDLE) {
            rcu_start_gp();
        }
        
        /* 让出 CPU */
        barrier();
        asm volatile("pause");
    }
}

/* 加速同步 (立即完成宽限期) */
void synchronize_rcu_expedited(void) {
    /* 向所有 CPU 发送 IPI 强制 QS */
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        if (i != smp_processor_id()) {
            smp_send_reschedule(i);
        }
    }
    
    synchronize_rcu();
}

/* ========== 回调处理 ========== */

/* 调用回调 */
static void rcu_invoke_cbs(struct rcu_data *rdp) {
    struct rcu_head *head;
    
    while (!list_empty(&rdp->done_cbs)) {
        head = list_first_entry(&rdp->done_cbs, struct rcu_head, list);
        list_del(&head->list);
        
        /* 调用回调函数 */
        if (head->func != NULL) {
            head->func(head);
        }
        
        rdp->n_cbs_invoked++;
    }
}

/* 处理 RCU 回调 */
void rcu_process_callbacks(void) {
    int cpu = smp_processor_id();
    struct rcu_data *rdp = &rcu_data[cpu];
    
    /* 检查是否需要报告 QS */
    if (rdp->core_needs_qs && rdp->cpu_no_qs) {
        /* 检查是否已退出 RCU 临界区 */
        if (this_cpu()->rcu_nesting == 0) {
            rcu_report_qs_rdp(cpu, rdp);
        }
    }
    
    /* 处理已完成的回调 */
    rcu_invoke_cbs(rdp);
}

/* 注册回调 */
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head)) {
    if (head == NULL) return;
    
    int cpu = smp_processor_id();
    struct rcu_data *rdp = &rcu_data[cpu];
    
    head->func = func;
    
    spin_lock(&rdp->mynode->lock);
    list_add(&head->list, &rdp->cblist);
    rdp->n_cbs++;
    spin_unlock(&rdp->mynode->lock);
    
    /* 确保宽限期在进行 */
    spin_lock(&rcu_state.lock);
    if (rcu_state.gp_state == RCU_GP_IDLE) {
        rcu_state.gp_state = RCU_GP_WAIT;
    }
    spin_unlock(&rcu_state.lock);
}

/* kfree_rcu 回调 */
void kfree_rcu_callback(struct rcu_head *head) {
    /* 获取包含 rcu_head 的结构体并释放 */
    void *ptr = (void *)((char *)head - offsetof(struct rcu_head, list));
    kfree(ptr);
}

/* ========== SRCU (可睡眠 RCU) ========== */

/* SRCU 读锁定 */
int srcu_read_lock(struct srcu_struct *sp) {
    if (sp == NULL) return 0;

    int idx = sp->completed & 0x1;
    int cpu = smp_processor_id();

    /* 使用原子操作避免竞态 */
    __sync_fetch_and_add(&sp->per_cpu_ref[cpu].lock_count[idx], 1);

    return idx;
}

/* SRCU 读解锁 */
void srcu_read_unlock(struct srcu_struct *sp, int idx) {
    if (sp == NULL) return;

    int cpu = smp_processor_id();
    __sync_fetch_and_add(&sp->per_cpu_ref[cpu].unlock_count[idx], 1);
}

/* SRCU 同步 */
void synchronize_srcu(struct srcu_struct *sp) {
    if (sp == NULL) return;

    int old_idx = sp->completed & 0x1;
    int new_idx = old_idx ^ 1;

    /* 等待所有读者完成 */
    int all_done;
    do {
        all_done = 1;
        for (uint32_t i = 0; i < smp_cpu_count; i++) {
            if (sp->per_cpu_ref[i].lock_count[old_idx] >
                sp->per_cpu_ref[i].unlock_count[old_idx]) {
                all_done = 0;
                break;
            }
        }
        if (!all_done) {
            barrier();
            asm volatile("pause");
        }
    } while (!all_done);

    /* 切换索引 */
    sp->completed++;
}

/* ========== SRCU 增强优化 ========== */

/* SRCU 初始化 (分配 per-CPU 引用计数) */
int srcu_init(struct srcu_struct *sp, const char *name) {
    if (sp == NULL) return -1;

    memset(sp, 0, sizeof(*sp));
    sp->completed = 0;
    sp->lock = SPINLOCK_INIT;
    INIT_LIST_HEAD(&sp->callback_queue);

    /* 分配 per-CPU 引用计数数组 */
    sp->per_cpu_ref = kmalloc(sizeof(struct srcu_array) * MAX_CPUS);
    if (sp->per_cpu_ref == NULL) {
        return -1;
    }

    memset(sp->per_cpu_ref, 0, sizeof(struct srcu_array) * MAX_CPUS);

    return 0;
}

/* SRCU 清理 */
void srcu_cleanup(struct srcu_struct *sp) {
    if (sp == NULL) return;

    /* 等待所有读者完成 */
    synchronize_srcu(sp);

    /* 处理剩余回调 */
    if (!list_empty(&sp->callback_queue)) {
        struct rcu_head *head, *tmp;
        list_for_each_entry_safe(head, tmp, &sp->callback_queue, list) {
            list_del(&head->list);
            if (head->func != NULL) {
                head->func(head);
            }
        }
    }

    if (sp->per_cpu_ref != NULL) {
        kfree(sp->per_cpu_ref);
        sp->per_cpu_ref = NULL;
    }
}

/* SRCU 异步回调 */
void call_srcu(struct srcu_struct *sp, struct rcu_head *head,
               void (*func)(struct rcu_head *head)) {
    if (sp == NULL || head == NULL) return;

    head->func = func;

    spin_lock(&sp->lock);
    list_add_tail(&head->list, &sp->callback_queue);
    spin_unlock(&sp->lock);
}

/* SRCU 加速同步 (减少等待时间) */
void synchronize_srcu_expedited(struct srcu_struct *sp) {
    if (sp == NULL) return;

    /* 向所有 CPU 发送 IPI 强制 QS 报告 */
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        if (i != smp_processor_id()) {
            smp_send_reschedule(i);
        }
    }

    synchronize_srcu(sp);
}

/* 获取 SRCU 状态信息 */
void srcu_get_stats(struct srcu_struct *sp, uint64_t *gp_count,
                    uint64_t *total_reads, uint64_t *pending_reads) {
    if (sp == NULL) return;

    if (gp_count != NULL) {
        *gp_count = sp->completed;
    }

    if (total_reads != NULL || pending_reads != NULL) {
        uint64_t total = 0, pending = 0;
        int idx = sp->completed & 0x1;

        for (uint32_t i = 0; i < smp_cpu_count; i++) {
            int locks = sp->per_cpu_ref[i].lock_count[idx];
            int unlocks = sp->per_cpu_ref[i].unlock_count[idx];
            total += locks;
            if (locks > unlocks) {
                pending += (locks - unlocks);
            }
        }

        if (total_reads != NULL) *total_reads = total;
        if (pending_reads != NULL) *pending_reads = pending;
    }
}

/* ========== CPU 热插拔 ========== */

int rcu_cpu_online(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) return -1;
    
    struct rcu_data *rdp = &rcu_data[cpu];
    
    spin_lock(&rcu_state.lock);
    rcu_state.n_online_cpus++;
    rdp->cpu_no_qs = 1;
    rdp->core_needs_qs = 0;
    spin_unlock(&rcu_state.lock);
    
    return 0;
}

int rcu_cpu_offline(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) return -1;
    
    struct rcu_data *rdp = &rcu_data[cpu];
    
    spin_lock(&rcu_state.lock);
    rcu_state.n_online_cpus--;
    
    /* 将回调移动到其他 CPU */
    if (!list_empty(&rdp->cblist)) {
        int target = (cpu + 1) % smp_cpu_count;
        struct rcu_data *target_rdp = &rcu_data[target];
        
        spin_lock(&target_rdp->mynode->lock);
        list_splice(&rdp->cblist, &target_rdp->cblist);
        target_rdp->n_cbs += rdp->n_cbs;
        spin_unlock(&target_rdp->mynode->lock);
        
        rdp->n_cbs = 0;
    }
    
    spin_unlock(&rcu_state.lock);
    
    return 0;
}

/* ========== 宽限期线程 ========== */

void rcu_gp_kthread(void) {
    while (1) {
        /* 检查是否需要启动宽限期 */
        spin_lock(&rcu_state.lock);
        int need_gp = !list_empty(&rcu_state.cblist) &&
                      rcu_state.gp_state == RCU_GP_IDLE;
        spin_unlock(&rcu_state.lock);
        
        if (need_gp) {
            rcu_start_gp();
        }
        
        /* 等待宽限期完成 */
        while (rcu_state.gp_state == RCU_GP_IN_PROGRESS) {
            /* 检查所有 CPU */
            if (rcu_implicit_cpu_qs_all()) {
                spin_lock(&rcu_state.lock);
                rcu_state.gp_seq++;
                rcu_state.gp_state = RCU_GP_IDLE;
                spin_unlock(&rcu_state.lock);
                break;
            }
            
            /* 让出 CPU */
            asm volatile("pause" ::: "memory");
        }
        
        /* 处理完成的回调 */
        for (uint32_t i = 0; i < smp_cpu_count; i++) {
            struct rcu_data *rdp = &rcu_data[i];
            
            if (!list_empty(&rdp->cblist)) {
                /* 将回调移动到完成列表 */
                spin_lock(&rdp->mynode->lock);
                list_splice(&rdp->cblist, &rdp->done_cbs);
                rdp->n_done_cbs += rdp->n_cbs;
                rdp->n_cbs = 0;
                spin_unlock(&rdp->mynode->lock);
            }
            
            rcu_invoke_cbs(rdp);
        }
    }
}

/* ========== 调试函数 ========== */

void rcu_print_stats(void) {
    println("[RCU] Statistics:");
    print("  Grace periods: ");
    print_dec(rcu_state.gp_seq);
    println("");
    print("  Online CPUs: ");
    print_dec(rcu_state.n_online_cpus);
    println("");
    
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        print("  CPU ");
        print_dec(i);
        print(" callbacks: ");
        print_dec(rcu_data[i].n_cbs);
        print(" invoked: ");
        print_dec(rcu_data[i].n_cbs_invoked);
        println("");
    }
}

/* ========== 批处理优化 ========== */

/* 回调批次 */
#define RCU_BATCH_SIZE      100
#define RCU_BATCH_DELAY_NS  1000000ULL  /* 1ms */

struct rcu_batch {
    struct rcu_head *heads[RCU_BATCH_SIZE];
    uint32_t count;
    uint64_t deadline;
};

static struct rcu_batch rcu_batch;

/* 批量注册回调 */
int call_rcu_batch(struct rcu_head **heads, uint32_t count) {
    if (heads == NULL || count == 0) return -1;
    
    int cpu = smp_processor_id();
    struct rcu_data *rdp = &rcu_data[cpu];
    
    spin_lock(&rdp->mynode->lock);
    
    for (uint32_t i = 0; i < count; i++) {
        if (heads[i] != NULL) {
            list_add(&heads[i]->list, &rdp->cblist);
            rdp->n_cbs++;
        }
    }
    
    spin_unlock(&rdp->mynode->lock);
    
    /* 启动宽限期 */
    spin_lock(&rcu_state.lock);
    if (rcu_state.gp_state == RCU_GP_IDLE) {
        rcu_state.gp_state = RCU_GP_WAIT;
    }
    spin_unlock(&rcu_state.lock);
    
    return 0;
}

/* 添加到批次 */
int rcu_batch_add(struct rcu_head *head, void (*func)(struct rcu_head *head)) {
    if (head == NULL) return -1;
    
    head->func = func;
    
    if (rcu_batch.count == 0) {
        rcu_batch.deadline = get_tick_count() + RCU_BATCH_DELAY_NS;
    }
    
    if (rcu_batch.count < RCU_BATCH_SIZE) {
        rcu_batch.heads[rcu_batch.count++] = head;
        return 0;
    }
    
    /* 批次已满，立即提交 */
    call_rcu_batch(rcu_batch.heads, rcu_batch.count);
    rcu_batch.count = 0;
    
    return 0;
}

/* 刷新批次 */
void rcu_batch_flush(void) {
    if (rcu_batch.count > 0) {
        call_rcu_batch(rcu_batch.heads, rcu_batch.count);
        rcu_batch.count = 0;
    }
}

/* ========== 延迟回调 ========== */

/* 延迟回调结构 */
struct rcu_delayed {
    struct rcu_head head;
    uint64_t deadline;          /* 执行时间 */
    uint32_t flags;
};

#define RCU_DELAYED_FLAG_PENDING   (1 << 0)
#define RCU_DELAYED_FLAG_EXECUTED  (1 << 1)

/* 延迟回调列表 */
static struct list_head delayed_cbs = LIST_HEAD_INIT(delayed_cbs);
static spinlock_t delayed_lock = SPINLOCK_INIT;

/* 注册延迟回调 */
int call_rcu_delayed(struct rcu_head *head, void (*func)(struct rcu_head *head),
                     uint64_t delay_ns) {
    if (head == NULL) return -1;
    
    struct rcu_delayed *dcb = container_of(head, struct rcu_delayed, head);
    dcb->head.func = func;
    dcb->deadline = get_tick_count() + delay_ns;
    dcb->flags = RCU_DELAYED_FLAG_PENDING;
    
    spin_lock(&delayed_lock);
    list_add(&dcb->head.list, &delayed_cbs);
    spin_unlock(&delayed_lock);
    
    return 0;
}

/* 处理延迟回调 */
void rcu_process_delayed(void) {
    uint64_t now = get_tick_count();
    
    spin_lock(&delayed_lock);
    
    struct rcu_delayed *dcb, *next;
    list_for_each_entry_safe(dcb, next, &delayed_cbs, head.list) {
        if (dcb->deadline <= now && (dcb->flags & RCU_DELAYED_FLAG_PENDING)) {
            dcb->flags &= ~RCU_DELAYED_FLAG_PENDING;
            dcb->flags |= RCU_DELAYED_FLAG_EXECUTED;
            
            /* 移动到 RCU 回调 */
            call_rcu(&dcb->head, dcb->head.func);
        }
    }
    
    spin_unlock(&delayed_lock);
}

/* ========== 回调优先级 ========== */

#define RCU_PRIO_LOW       0
#define RCU_PRIO_NORMAL    1
#define RCU_PRIO_HIGH      2
#define RCU_PRIO_COUNT     3

/* 优先级回调队列 */
struct rcu_prio_queue {
    struct list_head queues[RCU_PRIO_COUNT];
    uint32_t counts[RCU_PRIO_COUNT];
    spinlock_t lock;
};

static struct rcu_prio_queue rcu_pq;

/* 初始化优先级队列 */
void rcu_prio_init(void) {
    for (int i = 0; i < RCU_PRIO_COUNT; i++) {
        INIT_LIST_HEAD(&rcu_pq.queues[i]);
        rcu_pq.counts[i] = 0;
    }
    rcu_pq.lock = SPINLOCK_INIT;
}

/* 带优先级注册回调 */
int call_rcu_prio(struct rcu_head *head, void (*func)(struct rcu_head *head),
                  int prio) {
    if (head == NULL) return -1;
    if (prio < 0) prio = RCU_PRIO_LOW;
    if (prio >= RCU_PRIO_COUNT) prio = RCU_PRIO_HIGH;
    
    head->func = func;
    
    spin_lock(&rcu_pq.lock);
    list_add(&head->list, &rcu_pq.queues[prio]);
    rcu_pq.counts[prio]++;
    spin_unlock(&rcu_pq.lock);
    
    return 0;
}

/* 按优先级处理回调 */
void rcu_process_prio(void) {
    /* 从高到低处理 */
    for (int p = RCU_PRIO_HIGH; p >= RCU_PRIO_LOW; p--) {
        spin_lock(&rcu_pq.lock);
        
        while (!list_empty(&rcu_pq.queues[p])) {
            struct rcu_head *head = list_first_entry(&rcu_pq.queues[p], 
                                                     struct rcu_head, list);
            list_del(&head->list);
            rcu_pq.counts[p]--;
            
            if (head->func != NULL) {
                head->func(head);
            }
        }
        
        spin_unlock(&rcu_pq.lock);
    }
}

/* ========== 快速 RCU (Fast RCU) ========== */

/* 快速路径：无锁读端 */
static __thread int rcu_fast_nesting = 0;

void rcu_read_lock_fast(void) {
    rcu_fast_nesting++;
    barrier();
}

void rcu_read_unlock_fast(void) {
    barrier();
    rcu_fast_nesting--;
}

int rcu_read_lock_held_fast(void) {
    return rcu_fast_nesting > 0;
}

/* ========== RCU 诊断 ========== */

/* 检查死锁 */
int rcu_check_deadlock(void) {
    /* 检查是否有 CPU 长时间持有 RCU 读锁 */
    uint64_t now = get_tick_count();
    
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        struct rcu_data *rdp = &rcu_data[i];
        
        if (rdp->cpu_no_qs && rdp->core_needs_qs) {
            /* CPU 在等待 QS，检查是否超时 */
            if (rdp->mynode && now - rdp->mynode->gp_seq > 10000000000ULL) {
                /* 10 秒超时，可能死锁 */
                return i;  /* 返回问题 CPU */
            }
        }
    }
    
    return -1;  /* 无死锁 */
}

/* 获取 RCU 状态 */
void rcu_get_state(struct rcu_state *state) {
    *state = rcu_state;
}

/* RCU 性能统计 */
struct rcu_perf {
    uint64_t gp_count;          /* 宽限期数 */
    uint64_t cb_count;          /* 回调总数 */
    uint64_t avg_gp_duration;   /* 平均宽限期时长 */
    uint64_t max_gp_duration;   /* 最大宽限期时长 */
};

static struct rcu_perf rcu_perf;

void rcu_update_perf(uint64_t gp_duration) {
    rcu_perf.gp_count++;
    rcu_perf.avg_gp_duration = (rcu_perf.avg_gp_duration * 7 + gp_duration) / 8;
    if (gp_duration > rcu_perf.max_gp_duration) {
        rcu_perf.max_gp_duration = gp_duration;
    }
}

void rcu_get_perf(struct rcu_perf *perf) {
    *perf = rcu_perf;
}

/* ========== RCU 高级增强功能 ========== */

/* === RCU Boost (优先级提升) === */

/* RCU Boost 配置 */
#define RCU_BOOST_PRIO          1       /* Boost 后的优先级 */
#define RCU_BOOST_DELAY_MS      500     /* Boost 延迟 (ms) */

struct rcu_boost {
    uint32_t cpu;
    uint32_t orig_prio;
    uint64_t boost_time;
    int boosted;
    struct list_head list;
};

static struct list_head boost_list = LIST_HEAD_INIT(boost_list);
static spinlock_t boost_lock = SPINLOCK_INIT;
static uint64_t boost_counter;

/* 初始化 RCU Boost */
void rcu_boost_init(void) {
    boost_counter = 0;
}

/* 提升 RCU 读线程优先级 */
int rcu_boost_prio(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) return -1;
    
    struct rcu_boost *rb = (struct rcu_boost *)kmalloc(sizeof(struct rcu_boost));
    if (rb == NULL) return -1;
    
    rb->cpu = cpu;
    rb->orig_prio = 0;  /* 当前进程优先级 */
    rb->boost_time = get_tick_count();
    rb->boosted = 1;
    
    spin_lock(&boost_lock);
    list_add(&rb->list, &boost_list);
    boost_counter++;
    spin_unlock(&boost_lock);
    
    /* 设置进程优先级 */
    /* TODO: 实际设置进程优先级 */
    
    return 0;
}

/* 恢复 RCU 读线程优先级 */
void rcu_unboost_prio(int cpu) {
    spin_lock(&boost_lock);
    
    struct rcu_boost *rb, *next;
    list_for_each_entry_safe(rb, next, &boost_list, list) {
        if (rb->cpu == cpu && rb->boosted) {
            rb->boosted = 0;
            /* 恢复原始优先级 */
            /* TODO: 实际恢复进程优先级 */
            
            list_del(&rb->list);
            kfree(rb);
            break;
        }
    }
    
    spin_unlock(&boost_lock);
}

/* 检查需要 boost 的线程 */
void rcu_check_boost(void) {
    uint64_t now = get_tick_count();
    uint64_t threshold = RCU_BOOST_DELAY_MS * 1000000ULL;
    
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        struct rcu_data *rdp = &rcu_data[i];
        
        /* 如果线程长时间持有 RCU 读锁 */
        if (rdp->cpu_no_qs && rdp->core_needs_qs) {
            uint64_t wait_time = now - 0ULL;
            if (wait_time > threshold) {
                rcu_boost_prio(i);
            }
        }
    }
}

/* === RCU Stall 检测 === */

/* Stall 检测配置 */
#define RCU_STALL_WARN_MS       3000    /* 警告阈值 */
#define RCU_STALL_PANIC_MS      30000   /* Panic 阈值 */

struct rcu_stall_info {
    uint64_t last_progress;
    uint32_t stalled_cpu;
    uint32_t stall_count;
    int in_stall;
};

static struct rcu_stall_info stall_info;

/* 初始化 Stall 检测 */
void rcu_stall_init(void) {
    stall_info.last_progress = get_tick_count();
    stall_info.stalled_cpu = -1;
    stall_info.stall_count = 0;
    stall_info.in_stall = 0;
}

/* 检测 RCU Stall */
int rcu_check_stall(void) {
    uint64_t now = get_tick_count();
    uint64_t warn_threshold = RCU_STALL_WARN_MS * 1000000ULL;
    uint64_t panic_threshold = RCU_STALL_PANIC_MS * 1000000ULL;
    
    /* 检查宽限期是否卡住 */
    spin_lock(&rcu_state.lock);
    uint64_t gp_start_time = rcu_state.gp_start_time;
    int gp_state = rcu_state.gp_state;
    spin_unlock(&rcu_state.lock);
    
    if (gp_state == RCU_GP_IN_PROGRESS) {
        /* 使用正确的时间戳计算宽限期持续时间 */
        uint64_t gp_duration = now - gp_start_time;
        
        if (gp_duration > panic_threshold) {
            /* 严重 Stall，可能需要 panic */
            println("[RCU] CRITICAL: Grace period stalled for too long!");
            stall_info.in_stall = 2;
            return 2;
        }
        
        if (gp_duration > warn_threshold && !stall_info.in_stall) {
            /* 警告级别 Stall */
            println("[RCU] WARNING: Grace period taking too long");
            
            /* 找出阻塞的 CPU */
            for (uint32_t i = 0; i < smp_cpu_count; i++) {
                if (rcu_data[i].cpu_no_qs) {
                    stall_info.stalled_cpu = i;
                    print("[RCU] CPU ");
                    print_dec(i);
                    println(" has not passed quiescent state");
                    break;
                }
            }
            
            stall_info.stall_count++;
            stall_info.in_stall = 1;
            return 1;
        }
    } else {
        /* 宽限期完成，重置 stall 状态 */
        if (stall_info.in_stall) {
            stall_info.in_stall = 0;
            stall_info.last_progress = now;
        }
    }
    
    return 0;
}

/* === RCU Tree (层次化宽限期管理) === */

/* RCU Tree 节点 */
#define RCU_TREE_LEVELS     3
#define RCU_TREE_FANOUT     16

struct rcu_tree_node {
    uint64_t gp_seq;            /* 已完成的宽限期 */
    uint64_t gp_num;            /* 当前宽限期编号 */
    int level;
    int cpu_mask;               /* 子 CPU 掩码 */
    int qsmask;                 /* QS 状态掩码 */
    int grplo, grphi;           /* 组范围 */
    struct rcu_tree_node *parent;
    struct rcu_tree_node *children[RCU_TREE_FANOUT];
    spinlock_t lock;
};

static struct rcu_tree_node rcu_tree_root;
static struct rcu_tree_node rcu_tree_nodes[256];
static int rcu_tree_node_count;

/* 初始化 RCU Tree */
void rcu_tree_init(void) {
    rcu_tree_node_count = 0;
    memset(&rcu_tree_root, 0, sizeof(rcu_tree_root));
    rcu_tree_root.level = 0;
    rcu_tree_root.lock = SPINLOCK_INIT;
}

/* 构建 RCU Tree */
static void rcu_build_tree(int level, struct rcu_tree_node *parent, 
                           int cpu_start, int cpu_end) {
    if (cpu_start >= cpu_end) return;
    
    int node_idx = rcu_tree_node_count++;
    if (node_idx >= 256) return;
    
    struct rcu_tree_node *node = &rcu_tree_nodes[node_idx];
    memset(node, 0, sizeof(*node));
    node->level = level;
    node->grplo = cpu_start;
    node->grphi = cpu_end - 1;
    node->parent = parent;
    node->lock = SPINLOCK_INIT;
    
    if (parent != NULL) {
        for (int i = 0; i < RCU_TREE_FANOUT; i++) {
            if (parent->children[i] == NULL) {
                parent->children[i] = node;
                break;
            }
        }
    }
    
    /* 递归构建子节点 */
    int span = cpu_end - cpu_start;
    int child_span = (span + RCU_TREE_FANOUT - 1) / RCU_TREE_FANOUT;
    
    for (int i = 0; i < RCU_TREE_FANOUT && cpu_start < cpu_end; i++) {
        int child_end = cpu_start + child_span;
        if (child_end > cpu_end) child_end = cpu_end;
        
        if (level < RCU_TREE_LEVELS - 1) {
            rcu_build_tree(level + 1, node, cpu_start, child_end);
        }
        
        cpu_start = child_end;
    }
}

/* 在 Tree 中报告 QS */
void rcu_tree_report_qs(int cpu) {
    struct rcu_tree_node *node = &rcu_tree_nodes[cpu % 16 + 16];  /* 叶子节点 */
    
    while (node != NULL) {
        spin_lock(&node->lock);
        node->qsmask |= (1 << (cpu % RCU_TREE_FANOUT));
        
        /* 检查是否所有子节点都报告了 QS */
        if (node->qsmask == ((1 << RCU_TREE_FANOUT) - 1)) {
            node->qsmask = 0;
            node->gp_seq = rcu_tree_root.gp_num;
            spin_unlock(&node->lock);
            node = node->parent;
        } else {
            spin_unlock(&node->lock);
            break;
        }
    }
}

/* === NMI 安全 RCU === */

/* NMI 上下文中的 RCU 读锁定 */
static __thread int in_nmi_context = 0;

void rcu_nmi_enter(void) {
    in_nmi_context = 1;
    /* 禁用抢占 */
}

void rcu_nmi_exit(void) {
    in_nmi_context = 0;
    /* 恢复抢占 */
}

/* NMI 安全的读锁定 */
void rcu_read_lock_nmi_safe(void) {
    if (in_nmi_context) {
        /* NMI 上下文：无操作 (NMI 处理程序默认在 RCU 临界区) */
        return;
    }
    rcu_read_lock();
}

void rcu_read_unlock_nmi_safe(void) {
    if (in_nmi_context) {
        return;
    }
    rcu_read_unlock();
}

/* === RCU 统计与分析 === */

struct rcu_analysis {
    uint64_t total_gps;             /* 总宽限期数 */
    uint64_t total_callbacks;       /* 总回调数 */
    uint64_t stalled_count;         /* Stall 次数 */
    uint64_t boosted_count;         /* Boost 次数 */
    uint64_t avg_read_hold_time;    /* 平均读持锁时间 */
    uint64_t max_read_hold_time;    /* 最大读持锁时间 */
    uint64_t gp_durations[32];      /* 最近宽限期时长 */
    int gp_duration_idx;
};

static struct rcu_analysis rcu_analysis;

/* 记录宽限期时长 */
void rcu_record_gp_duration(uint64_t duration) {
    rcu_analysis.total_gps++;
    rcu_analysis.gp_durations[rcu_analysis.gp_duration_idx] = duration;
    rcu_analysis.gp_duration_idx = (rcu_analysis.gp_duration_idx + 1) % 32;
}

/* 记录读持锁时间 */
void rcu_record_read_hold(uint64_t hold_time) {
    rcu_analysis.avg_read_hold_time = 
        (rcu_analysis.avg_read_hold_time * 7 + hold_time) / 8;
    if (hold_time > rcu_analysis.max_read_hold_time) {
        rcu_analysis.max_read_hold_time = hold_time;
    }
}

/* 获取 RCU 分析数据 */
void rcu_get_analysis(struct rcu_analysis *analysis) {
    if (analysis != NULL) {
        *analysis = rcu_analysis;
    }
}

/* 打印 RCU 分析报告 */
void rcu_print_analysis(void) {
    println("=== RCU Analysis Report ===");
    
    print("Total grace periods: ");
    print_dec(rcu_analysis.total_gps);
    println("");
    
    print("Total callbacks: ");
    print_dec(rcu_analysis.total_callbacks);
    println("");
    
    print("Stall count: ");
    print_dec(rcu_analysis.stalled_count);
    println("");
    
    print("Boost count: ");
    print_dec(rcu_analysis.boosted_count);
    println("");
    
    print("Avg read hold time: ");
    print_dec(rcu_analysis.avg_read_hold_time);
    println(" ns");
    
    print("Max read hold time: ");
    print_dec(rcu_analysis.max_read_hold_time);
    println(" ns");
    
    /* 计算平均宽限期时长 */
    uint64_t sum = 0;
    int count = 0;
    for (int i = 0; i < 32; i++) {
        if (rcu_analysis.gp_durations[i] > 0) {
            sum += rcu_analysis.gp_durations[i];
            count++;
        }
    }
    
    if (count > 0) {
        print("Avg GP duration: ");
        print_dec(sum / count);
        println(" ns");
    }
}

/* === RCU 压力测试 === */

/* RCU 回调压力测试 */
static int rcu_stress_counter;

void rcu_stress_callback(struct rcu_head *head) {
    rcu_stress_counter++;
    kfree(head);
}

/* 运行 RCU 压力测试 */
void rcu_run_stress_test(int iterations) {
    println("[RCU] Running stress test...");
    
    rcu_stress_counter = 0;
    uint64_t start = get_tick_count();
    
    for (int i = 0; i < iterations; i++) {
        struct rcu_head *head = (struct rcu_head *)kmalloc(sizeof(struct rcu_head));
        if (head != NULL) {
            call_rcu(head, rcu_stress_callback);
        }
    }
    
    /* 等待所有回调完成 */
    synchronize_rcu();
    
    uint64_t end = get_tick_count();
    
    print("[RCU] Stress test completed: ");
    print_dec(rcu_stress_counter);
    print(" / ");
    print_dec(iterations);
    print(" callbacks in ");
    print_dec(end - start);
    println(" ns");
}

/* === RCU 懒惰回调 (Lazy RCU Callbacks) === */

/* 懒惰回调：延迟执行直到系统空闲 */
struct rcu_lazy {
    struct rcu_head head;
    uint64_t min_delay;
    uint64_t registered_time;
    int executed;
};

static struct list_head lazy_cbs = LIST_HEAD_INIT(lazy_cbs);
static spinlock_t lazy_lock = SPINLOCK_INIT;

/* 注册懒惰回调 */
int call_rcu_lazy(struct rcu_head *head, void (*func)(struct rcu_head *head),
                  uint64_t min_delay) {
    if (head == NULL) return -1;
    
    struct rcu_lazy *lazy = container_of(head, struct rcu_lazy, head);
    lazy->head.func = func;
    lazy->min_delay = min_delay;
    lazy->registered_time = get_tick_count();
    lazy->executed = 0;
    
    spin_lock(&lazy_lock);
    list_add(&lazy->head.list, &lazy_cbs);
    spin_unlock(&lazy_lock);
    
    return 0;
}

/* 处理懒惰回调 */
void rcu_process_lazy(void) {
    uint64_t now = get_tick_count();
    int system_idle = 1;  /* 简化：假设系统空闲 */
    
    spin_lock(&lazy_lock);
    
    struct rcu_lazy *lazy, *next;
    list_for_each_entry_safe(lazy, next, &lazy_cbs, head.list) {
        if (lazy->executed) continue;
        
        /* 检查最小延迟 */
        if (now - lazy->registered_time < lazy->min_delay) continue;
        
        /* 系统空闲时执行 */
        if (system_idle) {
            lazy->executed = 1;
            list_del(&lazy->head.list);
            call_rcu(&lazy->head, lazy->head.func);
        }
    }
    
    spin_unlock(&lazy_lock);
}

/* === RCU 事件追踪 === */

#define RCU_EVENT_MAX      1024

enum rcu_event_type {
    RCU_EVENT_GP_START,
    RCU_EVENT_GP_END,
    RCU_EVENT_CB_REGISTER,
    RCU_EVENT_CB_INVOKE,
    RCU_EVENT_STALL,
    RCU_EVENT_BOOST,
};

struct rcu_event {
    uint64_t timestamp;
    enum rcu_event_type type;
    uint32_t cpu;
    uint64_t data;
};

static struct rcu_event rcu_events[RCU_EVENT_MAX];
static uint32_t rcu_event_idx;
static int rcu_tracing_enabled;

/* 启用/禁用追踪 */
void rcu_trace_enable(int enable) {
    rcu_tracing_enabled = enable;
    if (enable) {
        rcu_event_idx = 0;
    }
}

/* 记录 RCU 事件 */
void rcu_trace_event(enum rcu_event_type type, uint32_t cpu, uint64_t data) {
    if (!rcu_tracing_enabled) return;

    uint32_t idx = __sync_fetch_and_add(&rcu_event_idx, 1) % RCU_EVENT_MAX;

    rcu_events[idx].timestamp = get_tick_count();
    rcu_events[idx].type = type;
    rcu_events[idx].cpu = cpu;
    rcu_events[idx].data = data;
}

/* 打印追踪事件 */
void rcu_print_trace(void) {
    println("=== RCU Event Trace ===");

    for (uint32_t i = 0; i < rcu_event_idx && i < RCU_EVENT_MAX; i++) {
        struct rcu_event *ev = &rcu_events[i];

        print("[");
        print_dec(ev->timestamp);
        print("] CPU");
        print_dec(ev->cpu);
        print(" ");

        switch (ev->type) {
            case RCU_EVENT_GP_START:
                print("GP_START seq=");
                break;
            case RCU_EVENT_GP_END:
                print("GP_END seq=");
                break;
            case RCU_EVENT_CB_REGISTER:
                print("CB_REGISTER count=");
                break;
            case RCU_EVENT_CB_INVOKE:
                print("CB_INVOKE n=");
                break;
            case RCU_EVENT_STALL:
                print("STALL cpu=");
                break;
            case RCU_EVENT_BOOST:
                print("BOOST prio=");
                break;
        }

        print_dec(ev->data);
        println("");
    }
}

/* ========== Grace Period 检测优化 ========== */

/*
 * 批量宽限期完成：合并多个宽限期的完成通知
 * 减少锁获取次数和 IPI 数量
 */
void rcu_batch_gp_complete(void) {
    spin_lock(&rcu_state.lock);

    if (rcu_state.gp_state != RCU_GP_IN_PROGRESS) {
        spin_unlock(&rcu_state.lock);
        return;
    }

    /* 批量检查所有 CPU 的 QS 状态 */
    int all_qs = 1;
    uint32_t pending_cpus = 0;

    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        struct rcu_data *rdp = &rcu_data[i];
        if (rdp->cpu_no_qs) {
            /* 检查是否已退出 RCU 临界区 */
            if (rdp->core_needs_qs) {
                all_qs = 0;
                pending_cpus++;
            } else {
                rdp->cpu_no_qs = 0;
            }
        }
    }

    if (all_qs) {
        /* 所有 CPU 都通过了 QS，批量完成宽限期 */
        uint64_t gp_duration = get_tick_count() - rcu_state.gp_start_time;
        rcu_state.gp_seq++;
        rcu_state.gp_state = RCU_GP_IDLE;
        rcu_update_perf(gp_duration);
        rcu_record_gp_duration(gp_duration);

        /* 批量移动回调到完成列表 */
        for (uint32_t i = 0; i < smp_cpu_count; i++) {
            struct rcu_data *rdp = &rcu_data[i];
            if (!list_empty(&rdp->cblist)) {
                list_splice(&rdp->cblist, &rdp->done_cbs);
                rdp->n_done_cbs += rdp->n_cbs;
                rdp->n_cbs = 0;
            }
        }
    }

    spin_unlock(&rcu_state.lock);
}

/*
 * 加速宽限期检测：使用更少的 IPI
 * 仅向尚未报告 QS 的 CPU 发送 IPI，而不是向所有 CPU 广播
 */
void rcu_accelerate_gp(void) {
    spin_lock(&rcu_state.lock);

    if (rcu_state.gp_state != RCU_GP_IN_PROGRESS) {
        spin_unlock(&rcu_state.lock);
        return;
    }

    /* 仅向未通过 QS 的 CPU 发送 IPI */
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        struct rcu_data *rdp = &rcu_data[i];
        if (rdp->cpu_no_qs && rdp->core_needs_qs && i != smp_processor_id()) {
            smp_send_reschedule(i);
        }
    }

    spin_unlock(&rcu_state.lock);
}

/* 获取当前宽限期状态信息 */
int rcu_gp_get_info(uint64_t *gp_seq, int *gp_state,
                    uint64_t *gp_duration, uint32_t *pending_cbs) {
    spin_lock(&rcu_state.lock);

    if (gp_seq != NULL) *gp_seq = rcu_state.gp_seq;
    if (gp_state != NULL) *gp_state = rcu_state.gp_state;

    if (gp_duration != NULL) {
        if (rcu_state.gp_state == RCU_GP_IN_PROGRESS) {
            *gp_duration = get_tick_count() - rcu_state.gp_start_time;
        } else {
            *gp_duration = 0;
        }
    }

    if (pending_cbs != NULL) {
        uint32_t total = 0;
        for (uint32_t i = 0; i < smp_cpu_count; i++) {
            total += (uint32_t)rcu_data[i].n_cbs;
        }
        *pending_cbs = total;
    }

    spin_unlock(&rcu_state.lock);
    return 0;
}

/* ========== RCU 回调批处理增强 ========== */

/* 回调批处理统计 */
struct rcu_cb_batch_stats {
    uint64_t total_batches;      /* 总批次数 */
    uint64_t total_cbs;          /* 总回调数 */
    uint64_t total_invoked;      /* 总调用数 */
    uint64_t avg_batch_size;     /* 平均批次大小 */
};

static struct rcu_cb_batch_stats cb_batch_stats;

/* 初始化回调批处理 */
void rcu_cb_batch_init(void) {
    memset(&cb_batch_stats, 0, sizeof(cb_batch_stats));
}

/* 批量处理回调：将多个回调合并到一次宽限期中处理 */
static void rcu_invoke_cbs_batch(struct rcu_data *rdp) {
    if (list_empty(&rdp->done_cbs)) {
        return;
    }

    /* 计算本批次回调数 */
    uint32_t batch_count = 0;
    struct rcu_head *head, *tmp;

    list_for_each_entry_safe(head, tmp, &rdp->done_cbs, list) {
        if (batch_count >= RCU_CB_BATCH_MAX_SIZE) {
            break;
        }

        list_del(&head->list);

        /* 调用回调函数 */
        if (head->func != NULL) {
            head->func(head);
        }

        batch_count++;
        rdp->n_cbs_invoked++;
    }

    /* 更新批处理统计 */
    cb_batch_stats.total_batches++;
    cb_batch_stats.total_cbs += batch_count;
    cb_batch_stats.total_invoked += batch_count;
    if (cb_batch_stats.total_batches > 0) {
        cb_batch_stats.avg_batch_size =
            cb_batch_stats.total_cbs / cb_batch_stats.total_batches;
    }
}

/* 刷新所有待处理的回调批次 */
void rcu_cb_batch_flush_all(void) {
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        struct rcu_data *rdp = &rcu_data[i];

        /* 先将待处理回调移动到完成列表 */
        if (!list_empty(&rdp->cblist)) {
            spin_lock(&rdp->mynode->lock);
            list_splice(&rdp->cblist, &rdp->done_cbs);
            rdp->n_done_cbs += rdp->n_cbs;
            rdp->n_cbs = 0;
            spin_unlock(&rdp->mynode->lock);
        }

        /* 批量调用回调 */
        rcu_invoke_cbs_batch(rdp);
    }
}

/* 获取回调批处理统计 */
void rcu_cb_batch_get_stats(uint64_t *total_batches, uint64_t *total_cbs,
                            uint64_t *avg_batch_size) {
    if (total_batches != NULL) *total_batches = cb_batch_stats.total_batches;
    if (total_cbs != NULL) *total_cbs = cb_batch_stats.total_cbs;
    if (avg_batch_size != NULL) *avg_batch_size = cb_batch_stats.avg_batch_size;
}
