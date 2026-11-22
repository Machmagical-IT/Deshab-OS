/*
 * Zratro OS - CFS (Completely Fair Scheduler) Implementation
 * 
 * Linux 风格的完全公平调度器
 */

#include "cfs.h"
#include "pmm.h"
#include "vmm.h"
#include "console.h"
#include "lockdep.h"
#include "string.h"
#include "smp.h"
#include "apic.h"

/* ========== 红黑树实现 ========== */

#define RB_RED      0
#define RB_BLACK    1

/* 旋转操作 */
static void rb_rotate_left(struct rb_root *root, struct rb_node *node) {
    struct rb_node *right = node->right;
    
    node->right = right->left;
    if (right->left != NULL) {
        right->left->parent = node;
    }
    
    right->parent = node->parent;
    if (node->parent == NULL) {
        root->rb_node = right;
    } else if (node == node->parent->left) {
        node->parent->left = right;
    } else {
        node->parent->right = right;
    }
    
    right->left = node;
    node->parent = right;
}

static void rb_rotate_right(struct rb_root *root, struct rb_node *node) {
    struct rb_node *left = node->left;
    
    node->left = left->right;
    if (left->right != NULL) {
        left->right->parent = node;
    }
    
    left->parent = node->parent;
    if (node->parent == NULL) {
        root->rb_node = left;
    } else if (node == node->parent->right) {
        node->parent->right = left;
    } else {
        node->parent->left = left;
    }
    
    left->right = node;
    node->parent = left;
}

/* 插入修复 */
static void rb_insert_fixup(struct rb_root *root, struct rb_node *node) {
    while (node->parent != NULL && node->parent->color == RB_RED) {
        struct rb_node *grandparent = node->parent->parent;
        
        if (node->parent == grandparent->left) {
            struct rb_node *uncle = grandparent->right;
            
            if (uncle != NULL && uncle->color == RB_RED) {
                node->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                grandparent->color = RB_RED;
                node = grandparent;
            } else {
                if (node == node->parent->right) {
                    node = node->parent;
                    rb_rotate_left(root, node);
                }
                node->parent->color = RB_BLACK;
                grandparent->color = RB_RED;
                rb_rotate_right(root, grandparent);
            }
        } else {
            struct rb_node *uncle = grandparent->left;
            
            if (uncle != NULL && uncle->color == RB_RED) {
                node->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                grandparent->color = RB_RED;
                node = grandparent;
            } else {
                if (node == node->parent->left) {
                    node = node->parent;
                    rb_rotate_right(root, node);
                }
                node->parent->color = RB_BLACK;
                grandparent->color = RB_RED;
                rb_rotate_left(root, grandparent);
            }
        }
    }
    
    root->rb_node->color = RB_BLACK;
}

/* 删除修复 */
static void rb_erase_fixup(struct rb_root *root, struct rb_node *node,
                           struct rb_node *parent, int is_left) {
    while (node != root->rb_node && (node == NULL || node->color == RB_BLACK)) {
        if (is_left) {
            struct rb_node *sibling = parent->right;
            
            if (sibling != NULL && sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color = RB_RED;
                rb_rotate_left(root, parent);
                sibling = parent->right;
            }
            
            if ((sibling->left == NULL || sibling->left->color == RB_BLACK) &&
                (sibling->right == NULL || sibling->right->color == RB_BLACK)) {
                sibling->color = RB_RED;
                node = parent;
                parent = node->parent;
                if (parent != NULL) {
                    is_left = (node == parent->left);
                }
            } else {
                if (sibling->right == NULL || sibling->right->color == RB_BLACK) {
                    sibling->left->color = RB_BLACK;
                    sibling->color = RB_RED;
                    rb_rotate_right(root, sibling);
                    sibling = parent->right;
                }
                
                sibling->color = parent->color;
                parent->color = RB_BLACK;
                sibling->right->color = RB_BLACK;
                rb_rotate_left(root, parent);
                node = root->rb_node;
            }
        } else {
            struct rb_node *sibling = parent->left;
            
            if (sibling != NULL && sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color = RB_RED;
                rb_rotate_right(root, parent);
                sibling = parent->left;
            }
            
            if ((sibling->right == NULL || sibling->right->color == RB_BLACK) &&
                (sibling->left == NULL || sibling->left->color == RB_BLACK)) {
                sibling->color = RB_RED;
                node = parent;
                parent = node->parent;
                if (parent != NULL) {
                    is_left = (node == parent->left);
                }
            } else {
                if (sibling->left == NULL || sibling->left->color == RB_BLACK) {
                    sibling->right->color = RB_BLACK;
                    sibling->color = RB_RED;
                    rb_rotate_left(root, sibling);
                    sibling = parent->left;
                }
                
                sibling->color = parent->color;
                parent->color = RB_BLACK;
                sibling->left->color = RB_BLACK;
                rb_rotate_right(root, parent);
                node = root->rb_node;
            }
        }
    }
    
    if (node != NULL) {
        node->color = RB_BLACK;
    }
}

/* 插入节点 */
void rb_insert(struct rb_root *root, struct rb_node *node) {
    node->left = NULL;
    node->right = NULL;
    node->color = RB_RED;
    
    /* 查找插入位置 */
    struct rb_node *parent = NULL;
    struct rb_node **link = &root->rb_node;
    
    while (*link != NULL) {
        parent = *link;
        if (node->key < (*link)->key) {
            link = &(*link)->left;
        } else {
            link = &(*link)->right;
        }
    }
    
    node->parent = parent;
    *link = node;
    
    /* 更新最左节点 */
    if (root->rb_leftmost == NULL || node->key < root->rb_leftmost->key) {
        root->rb_leftmost = node;
    }
    
    /* 修复红黑树性质 */
    rb_insert_fixup(root, node);
    
    root->nr_nodes++;
}

/* 删除节点 */
void rb_erase(struct rb_root *root, struct rb_node *node) {
    struct rb_node *child;
    struct rb_node *parent;
    int color;
    int is_left = 0;
    
    /* 更新最左节点 */
    if (node == root->rb_leftmost) {
        root->rb_leftmost = rb_next(node);
    }
    
    if (node->left == NULL) {
        child = node->right;
    } else if (node->right == NULL) {
        child = node->left;
    } else {
        /* 两个子节点：找后继 */
        struct rb_node *successor = node->right;
        while (successor->left != NULL) {
            successor = successor->left;
        }
        
        child = successor->right;
        parent = successor->parent;
        color = successor->color;
        
        if (child != NULL) {
            child->parent = parent;
        }
        
        if (parent != NULL) {
            if (successor == parent->left) {
                parent->left = child;
                is_left = 1;
            } else {
                parent->right = child;
            }
        }
        
        if (successor->parent == node) {
            parent = successor;
        }
        
        /* 替换节点 */
        successor->left = node->left;
        successor->right = node->right;
        successor->parent = node->parent;
        successor->color = node->color;
        
        if (node->left != NULL) {
            node->left->parent = successor;
        }
        if (node->right != NULL) {
            node->right->parent = successor;
        }
        
        if (node->parent != NULL) {
            if (node == node->parent->left) {
                node->parent->left = successor;
            } else {
                node->parent->right = successor;
            }
        } else {
            root->rb_node = successor;
        }
        
        goto fixup;
    }
    
    parent = node->parent;
    color = node->color;
    
    if (child != NULL) {
        child->parent = parent;
    }
    
    if (parent != NULL) {
        if (node == parent->left) {
            parent->left = child;
            is_left = 1;
        } else {
            parent->right = child;
        }
    } else {
        root->rb_node = child;
    }
    
fixup:
    if (color == RB_BLACK && child != NULL) {
        rb_erase_fixup(root, child, parent, is_left);
    }
    
    root->nr_nodes--;
}

/* 获取最小节点 */
struct rb_node *rb_first(struct rb_root *root) {
    return root->rb_leftmost;
}

/* 获取下一个节点 */
struct rb_node *rb_next(struct rb_node *node) {
    if (node->right != NULL) {
        node = node->right;
        while (node->left != NULL) {
            node = node->left;
        }
        return node;
    }
    
    struct rb_node *parent = node->parent;
    while (parent != NULL && node == parent->right) {
        node = parent;
        parent = parent->parent;
    }
    
    return parent;
}

/* ========== CFS 核心实现 ========== */

/* 获取当前时间 (纳秒) */
static uint64_t cfs_get_time(void) {
    /* 使用 TSC 获取时间 */
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    
    /* 假设 TSC 频率为 CPU 频率，这里简化处理 */
    /* 实际应该校准 TSC 频率 */
    return tsc * 1000000000ULL / 3000000000ULL;  /* 假设 3GHz */
}

/* 初始化 CFS 运行队列 */
void cfs_rq_init(struct cfs_rq *cfs_rq, int cpu) {
    memset(cfs_rq, 0, sizeof(*cfs_rq));
    
    cfs_rq->tasks_timeline.rb_node = NULL;
    cfs_rq->tasks_timeline.rb_leftmost = NULL;
    cfs_rq->tasks_timeline.nr_nodes = 0;
    
    cfs_rq->curr = NULL;
    cfs_rq->next = NULL;
    cfs_rq->last = NULL;
    cfs_rq->skip = NULL;
    
    cfs_rq->load_weight = 0;
    cfs_rq->runnable_weight = 0;
    cfs_rq->load_weight_recip = 0;
    cfs_rq->load_weight_shift = 0;
    cfs_rq->nr_running = 0;
    cfs_rq->h_nr_running = 0;
    
    cfs_rq->min_vruntime = 0;
    cfs_rq->exec_clock = 0;
    cfs_rq->clock = 0;
    cfs_rq->clock_task = 0;
    
    cfs_rq->sched_latency = CFS_SCHED_LATENCY;
    cfs_rq->min_granularity = CFS_MIN_GRANULARITY;
    
    cfs_rq->lock = 0;
    cfs_rq->cpu = cpu;
}

/* 全局 CFS 初始化 */
void cfs_init(void) {
    println("[CFS] Initializing Completely Fair Scheduler...");
    
    /* 初始化 Per-CPU 运行队列 */
    extern struct cfs_rq cfs_rqs[];
    for (int i = 0; i < MAX_CPUS; i++) {
        cfs_rq_init(&cfs_rqs[i], i);
    }
    
    println("[CFS] CFS initialized successfully");
}

/* 更新 load_weight 逆权重 (在 load_weight 变化时调用) */
static void cfs_update_load_recip(struct cfs_rq *cfs_rq) {
    if (cfs_rq->load_weight == 0) {
        cfs_rq->load_weight_recip = 0;
        cfs_rq->load_weight_shift = 0;
        return;
    }
    /* 计算 fls(load_weight) 来选择合适的移位量 */
    int shift = 0;
    uint64_t w = cfs_rq->load_weight;
    while (w >= (1ULL << 32)) {
        w >>= 1;
        shift++;
    }
    cfs_rq->load_weight_shift = shift + 32;
    cfs_rq->load_weight_recip = (1ULL << (shift + 32)) / cfs_rq->load_weight;
}

/* 快速除以 load_weight: a / load_weight ≈ (a * recip) >> shift */
static inline uint64_t div_load_weight(struct cfs_rq *cfs_rq, uint64_t val) {
    if (unlikely(cfs_rq->load_weight == 0)) return 0;
    uint64_t tmp = val * cfs_rq->load_weight_recip;
    tmp >>= cfs_rq->load_weight_shift;
    return tmp;
}

/* 自适应调度周期 (与 Linux sched_period 一致) */
static uint64_t sched_period(struct cfs_rq *cfs_rq) {
    uint64_t period = cfs_rq->sched_latency;
    uint32_t nr_running = cfs_rq->nr_running;

    if (nr_running > 0) {
        if (period / nr_running < cfs_rq->min_granularity)
            period = (uint64_t)nr_running * cfs_rq->min_granularity;
    }
    return period;
}

/* 计算时间片 */
uint64_t cfs_calc_slice(struct cfs_rq *cfs_rq, struct sched_entity *se) {
    uint64_t slice;

    if (unlikely(cfs_rq->nr_running == 0)) {
        return CFS_DEFAULT_SLICE;
    }

    /* slice = sched_period * weight / total_weight (使用逆权重乘法) */
    slice = sched_period(cfs_rq) * se->weight;
    slice = div_load_weight(cfs_rq, slice);

    /* 限制最小和最大值 */
    if (slice < CFS_MIN_SLICE) {
        slice = CFS_MIN_SLICE;
    }
    if (slice > CFS_MAX_SLICE) {
        slice = CFS_MAX_SLICE;
    }

    return slice;
}

/* 逆权重乘法替代除法 (与 Linux __calc_delta 一致) */
uint64_t __calc_delta(uint64_t delta_exec, uint32_t wmult_idx) {
    uint32_t wmult = sched_prio_to_wmult[wmult_idx];

    /* 逆权重乘法: delta * wmult >> 32 (近似 2^32/weight 乘法) */
    uint64_t tmp = delta_exec * wmult;
    tmp >>= 32;

    return tmp;
}

/* max_vruntime: 保守的单调递增更新 (与 Linux 一致) */
static inline uint64_t max_vruntime(uint64_t a, uint64_t b) {
    return (suseconds_t)(a - b) < 0 ? b : a;
}

/* 更新虚拟运行时间 */
void cfs_update_vruntime(struct cfs_rq *cfs_rq, struct sched_entity *se, uint64_t delta_exec) {
    if (unlikely(se == NULL)) return;

    /* 计算虚拟时间增量 */
    uint64_t vruntime_delta = __calc_delta(delta_exec, se->wmult_idx);

    /* 更新累计执行时间 */
    se->sum_exec_runtime += delta_exec;
    se->vruntime += vruntime_delta;

    /* 保守更新最小虚拟时间: 只在 vruntime 更大时才更新 */
    cfs_rq->min_vruntime = max_vruntime(cfs_rq->min_vruntime, se->vruntime);
}

/* 入队 */
void cfs_enqueue_task(struct cfs_rq *cfs_rq, struct sched_entity *se) {
    if (unlikely(se == NULL || se->on_rq)) return;

    spin_lock(&cfs_rq->lock);

    /* 初始化虚拟时间 */
    if (se->vruntime == 0) {
        se->vruntime = cfs_rq->min_vruntime;
    }

    /* 设置红黑树键值 */
    se->run_node.key = se->vruntime;

    /* 插入红黑树 */
    rb_insert(&cfs_rq->tasks_timeline, &se->run_node);

    se->on_rq = 1;
    cfs_rq->nr_running++;
    cfs_rq->load_weight += se->weight;
    cfs_rq->runnable_weight += se->weight;
    cfs_update_load_recip(cfs_rq);

    /* 更新时间片 */
    se->slice = cfs_calc_slice(cfs_rq, se);

    spin_unlock(&cfs_rq->lock);
}

/* 出队 */
void cfs_dequeue_task(struct cfs_rq *cfs_rq, struct sched_entity *se) {
    if (unlikely(se == NULL || !se->on_rq)) return;

    spin_lock(&cfs_rq->lock);

    rb_erase(&cfs_rq->tasks_timeline, &se->run_node);

    se->on_rq = 0;
    cfs_rq->nr_running--;
    cfs_rq->load_weight -= se->weight;
    cfs_rq->runnable_weight -= se->weight;
    cfs_update_load_recip(cfs_rq);

    if (cfs_rq->curr == se) {
        cfs_rq->curr = NULL;
    }

    spin_unlock(&cfs_rq->lock);
}

/* 选择下一个任务 */
struct sched_entity *cfs_pick_next_task(struct cfs_rq *cfs_rq) {
    struct rb_node *node = rb_first(&cfs_rq->tasks_timeline);

    if (unlikely(node == NULL)) {
        return NULL;
    }

    return container_of(node, struct sched_entity, run_node);
}

/* 放回当前任务 */
void cfs_put_prev_task(struct cfs_rq *cfs_rq, struct sched_entity *se) {
    if (unlikely(se == NULL)) return;

    /* 更新虚拟时间 */
    uint64_t now = cfs_get_time();
    uint64_t delta = now - cfs_rq->clock;

    if (likely(delta > 0)) {
        cfs_update_vruntime(cfs_rq, se, delta);
    }

    cfs_rq->curr = NULL;

    /* 如果任务还在运行状态，重新入队 */
    if (likely(se->on_rq)) {
        rb_erase(&cfs_rq->tasks_timeline, &se->run_node);
        se->run_node.key = se->vruntime;
        rb_insert(&cfs_rq->tasks_timeline, &se->run_node);
    }
}

/* 时钟 tick */
void cfs_task_tick(struct cfs_rq *cfs_rq, struct sched_entity *se) {
    if (unlikely(se == NULL)) return;

    /* 更新虚拟时间 */
    uint64_t now = cfs_get_time();
    uint64_t delta = now - cfs_rq->clock;
    cfs_rq->clock = now;

    if (likely(delta > 0 && se == cfs_rq->curr)) {
        cfs_update_vruntime(cfs_rq, se, delta);
    }

    /* 检查时间片是否用完 */
    if (se->sum_exec_runtime - se->prev_sum_exec_runtime >= se->slice) {
        if (likely(se->thread != NULL)) {
            se->thread->flags |= THREAD_FLAG_NEED_RESCHED;
        }
    }

    /* 检查是否需要抢占 (有新任务更公平) */
    struct rb_node *leftmost = rb_first(&cfs_rq->tasks_timeline);
    if (leftmost != NULL) {
        struct sched_entity *next = container_of(leftmost, struct sched_entity, run_node);

        if (next->vruntime + CFS_WAKEUP_GRANULARITY < se->vruntime) {
            if (likely(se->thread != NULL)) {
                se->thread->flags |= THREAD_FLAG_NEED_RESCHED;
            }
        }
    }
}

/* 唤醒时检查抢占 */
int cfs_check_preempt_wakeup(struct cfs_rq *cfs_rq, struct sched_entity *curr,
                              struct sched_entity *se) {
    if (unlikely(curr == NULL || se == NULL)) return 0;
    
    /* 如果唤醒的任务虚拟时间更小，可以抢占 */
    if (se->vruntime + CFS_WAKEUP_GRANULARITY < curr->vruntime) {
        return 1;
    }
    
    return 0;
}

/* 设置 nice 值 */
void cfs_set_nice(struct sched_entity *se, int nice) {
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;

    se->nice = nice;
    se->weight = sched_weight(nice);
    se->wmult_idx = sched_wmult_idx(nice);
}

/* 初始化调度实体 */
void cfs_init_entity(struct sched_entity *se, struct thread *thread) {
    memset(se, 0, sizeof(*se));

    se->thread = thread;
    se->nice = 0;
    se->weight = NICE_0_LOAD;
    se->wmult_idx = 20;  /* nice=0 → idx=20 */
    se->slice = CFS_DEFAULT_SLICE;
    se->on_rq = 0;
    se->vruntime = 0;
}

/* 负载均衡 */
void cfs_load_balance(struct cfs_rq *src, struct cfs_rq *dst) {
    if (src == NULL || dst == NULL) return;
    if (src->nr_running <= dst->nr_running + 1) return;
    
    /* 使用安全的双锁获取，避免死锁 */
    double_spin_lock(&src->lock, &dst->lock);
    
    /* 从源队列移动任务到目标队列 */
    struct rb_node *node = rb_first(&src->tasks_timeline);
    if (node != NULL) {
        struct sched_entity *se = container_of(node, struct sched_entity, run_node);
        
        /* 从源出队 */
        rb_erase(&src->tasks_timeline, node);
        src->nr_running--;
        src->load_weight -= se->weight;
        cfs_update_load_recip(src);
        
        /* 调整虚拟时间 */
        if (se->vruntime < dst->min_vruntime) {
            se->vruntime = dst->min_vruntime;
        }
        
        /* 入队到目标 */
        se->run_node.key = se->vruntime;
        rb_insert(&dst->tasks_timeline, &se->run_node);
        dst->nr_running++;
        dst->load_weight += se->weight;
        cfs_update_load_recip(dst);
    }
    
    /* 使用安全的双锁释放 */
    double_spin_unlock(&src->lock, &dst->lock);
}

/* Per-CPU CFS 运行队列 */
struct cfs_rq cfs_rqs[MAX_CPUS];

/* 获取当前 CPU 的 CFS 运行队列 */
struct cfs_rq *this_cfs_rq(void) {
    return &cfs_rqs[smp_processor_id()];
}

/* ========== NUMA 感知负载均衡 ========== */

/* NUMA 节点信息 */
struct numa_node {
    int node_id;
    uint64_t cpu_mask;          /* 该节点上的 CPU 掩码 */
    uint64_t load;              /* 当前负载 */
    uint64_t capacity;          /* 容量 */
    spinlock_t lock;
};

#define MAX_NUMA_NODES  8
static struct numa_node numa_nodes[MAX_NUMA_NODES];
static int numa_node_count = 0;

/* 初始化 NUMA 信息 */
void cfs_numa_init(void) {
    /* 简化：假设单 NUMA 节点 */
    numa_nodes[0].node_id = 0;
    numa_nodes[0].cpu_mask = 0xFF;  /* CPU 0-7 */
    numa_nodes[0].load = 0;
    numa_nodes[0].capacity = 1000;
    numa_nodes[0].lock = SPINLOCK_INIT;
    numa_node_count = 1;
    
    println("[CFS] NUMA initialized: 1 node");
}

/* 获取 CPU 所属的 NUMA 节点 */
static int cpu_to_node(int cpu) {
    for (int i = 0; i < numa_node_count; i++) {
        if (numa_nodes[i].cpu_mask & (1ULL << cpu)) {
            return i;
        }
    }
    return 0;
}

/* NUMA 感知的负载均衡 */
void cfs_numa_balance(int src_cpu, int dst_cpu) {
    int src_node = cpu_to_node(src_cpu);
    int dst_node = cpu_to_node(dst_cpu);
    
    /* 如果是同一 NUMA 节点，直接迁移 */
    if (src_node == dst_node) {
        cfs_load_balance(&cfs_rqs[src_cpu], &cfs_rqs[dst_cpu]);
        return;
    }
    
    /* 跨 NUMA 节点迁移：检查负载差异 */
    struct numa_node *src = &numa_nodes[src_node];
    struct numa_node *dst = &numa_nodes[dst_node];
    
    /* 使用安全的双锁获取，避免死锁 */
    double_spin_lock(&src->lock, &dst->lock);
    
    /* 只有目标节点负载明显低于源节点时才迁移 */
    if (dst->load * 120 < src->load * 100) {
        cfs_load_balance(&cfs_rqs[src_cpu], &cfs_rqs[dst_cpu]);
        
        /* 更新节点负载 */
        src->load -= 100;
        dst->load += 100;
    }
    
    /* 使用安全的双锁释放 */
    double_spin_unlock(&src->lock, &dst->lock);
}

/* 选择最空闲的 CPU */
int cfs_select_idle_cpu(struct sched_entity *se, int prev_cpu) {
    int best_cpu = prev_cpu;
    uint64_t best_load = UINT64_MAX;
    
    /* 首先在同 NUMA 节点内查找 */
    int node = cpu_to_node(prev_cpu);
    
    for (int i = 0; i < MAX_CPUS; i++) {
        if (!(numa_nodes[node].cpu_mask & (1ULL << i))) continue;
        
        uint64_t load = cfs_rqs[i].nr_running * 100 + cfs_rqs[i].load_weight / 1024;
        
        if (load < best_load) {
            best_load = load;
            best_cpu = i;
        }
    }
    
    /* 如果同节点内负载都很高，考虑跨节点 */
    if (best_load > 500 && numa_node_count > 1) {
        for (int n = 0; n < numa_node_count; n++) {
            if (n == node) continue;
            
            for (int i = 0; i < MAX_CPUS; i++) {
                if (!(numa_nodes[n].cpu_mask & (1ULL << i))) continue;
                
                uint64_t load = cfs_rqs[i].nr_running * 100 + cfs_rqs[i].load_weight / 1024;
                
                /* 跨节点需要额外代价 */
                load += 50;
                
                if (load < best_load) {
                    best_load = load;
                    best_cpu = i;
                }
            }
        }
    }
    
    return best_cpu;
}

/* ========== 带宽控制 (CFS Bandwidth) ========== */

/* 任务组带宽 */
struct cfs_bandwidth {
    uint64_t quota;             /* 配额 (纳秒) */
    uint64_t period;            /* 周期 (纳秒) */
    uint64_t runtime;           /* 剩余运行时间 */
    uint64_t expires;           /* 过期时间 */
    spinlock_t lock;
};

/* 默认带宽：无限制 */
static struct cfs_bandwidth global_bandwidth = {
    .quota = UINT64_MAX,
    .period = 100000000ULL,    /* 100ms */
    .runtime = UINT64_MAX,
    .expires = 0,
    .lock = SPINLOCK_INIT
};

/* 设置任务组带宽 */
int cfs_set_bandwidth(uint64_t quota, uint64_t period) {
    spin_lock(&global_bandwidth.lock);
    
    global_bandwidth.quota = quota;
    global_bandwidth.period = period;
    global_bandwidth.runtime = quota;
    
    spin_unlock(&global_bandwidth.lock);
    return 0;
}

/* 检查是否超出带宽 */
int cfs_check_bandwidth(struct sched_entity *se, uint64_t delta) {
    spin_lock(&global_bandwidth.lock);
    
    if (global_bandwidth.runtime < delta) {
        /* 超出配额，需要节流 */
        spin_unlock(&global_bandwidth.lock);
        return -1;  /* 需要等待下一个周期 */
    }
    
    global_bandwidth.runtime -= delta;
    
    spin_unlock(&global_bandwidth.lock);
    return 0;
}

/* 补充带宽 */
void cfs_refill_bandwidth(void) {
    uint64_t now = cfs_get_time();
    
    if (now < global_bandwidth.expires) return;
    
    spin_lock(&global_bandwidth.lock);
    
    global_bandwidth.runtime = global_bandwidth.quota;
    global_bandwidth.expires = now + global_bandwidth.period;
    
    spin_unlock(&global_bandwidth.lock);
}

/* ========== 组调度支持 ========== */

/* 任务组 */
struct task_group {
    struct cfs_rq *cfs_rq;          /* 组的 CFS 运行队列 */
    struct sched_entity *se;        /* 组作为实体的表示 */
    uint64_t shares;                /* CPU 份额 */
    int parent;                     /* 父组索引 */
    int level;                      /* 层级深度 */
};

#define MAX_TASK_GROUPS  64
static struct task_group task_groups[MAX_TASK_GROUPS];
static int task_group_count = 1;  /* 0 是根组 */

/* 创建任务组 */
int cfs_create_task_group(int parent) {
    if (task_group_count >= MAX_TASK_GROUPS) return -1;
    
    int idx = task_group_count++;
    
    task_groups[idx].shares = 1024;
    task_groups[idx].parent = parent;
    task_groups[idx].level = task_groups[parent].level + 1;
    
    return idx;
}

/* 设置任务组份额 */
void cfs_set_group_shares(int group, uint64_t shares) {
    if (group >= 0 && group < task_group_count) {
        task_groups[group].shares = shares;
    }
}

/* ========== 性能统计 ========== */

struct cfs_stats {
    uint64_t context_switches;
    uint64_t migrations;
    uint64_t wakeups;
    uint64_t preempt_count;
    uint64_t avg_slice;
    uint64_t max_slice;
    uint64_t min_slice;
};

static struct cfs_stats cfs_stats;

void cfs_record_switch(uint64_t slice) {
    cfs_stats.context_switches++;
    
    cfs_stats.avg_slice = (cfs_stats.avg_slice * 7 + slice) / 8;
    
    if (slice > cfs_stats.max_slice || cfs_stats.max_slice == 0) {
        cfs_stats.max_slice = slice;
    }
    if (slice < cfs_stats.min_slice || cfs_stats.min_slice == 0) {
        cfs_stats.min_slice = slice;
    }
}

void cfs_get_stats(struct cfs_stats *stats) {
    *stats = cfs_stats;
}

/* ========== CFS 高级增强功能 ========== */

/* === CFS 动态优先级调整 === */

/* 根据进程行为动态调整优先级 */
#define DYNAMIC_PRIO_SAMPLES    8
#define DYNAMIC_PRIO_DECAY      7  /* 指数移动平均衰减因子 */

struct dynamic_prio_data {
    uint64_t sleep_avg;           /* 平均睡眠时间 */
    uint64_t run_avg;             /* 平均运行时间 */
    uint64_t last_sleep;          /* 上次睡眠时间 */
    uint64_t last_run;            /* 上次运行时间 */
    int bonus;                    /* 优先级奖励 (-5 到 +5) */
    int interactive;              /* 是否为交互式任务 */
};

static struct dynamic_prio_data dynamic_prio[MAX_THREADS];

/* 更新睡眠时间 */
void cfs_update_sleep(struct sched_entity *se, uint64_t sleep_time) {
    if (se == NULL || se->thread == NULL) return;
    
    uint32_t tid = se->thread->tid;
    struct dynamic_prio_data *dp = &dynamic_prio[tid % MAX_THREADS];
    
    /* 指数移动平均 */
    dp->sleep_avg = (dp->sleep_avg * DYNAMIC_PRIO_DECAY + sleep_time) / 
                    (DYNAMIC_PRIO_DECAY + 1);
    dp->last_sleep = sleep_time;
    
    /* 计算交互性分数 */
    if (dp->sleep_avg > dp->run_avg * 2) {
        dp->interactive = 1;
        dp->bonus = 5;  /* 交互式任务获得优先级奖励 */
    } else if (dp->sleep_avg > dp->run_avg) {
        dp->bonus = 2;
    } else if (dp->run_avg > dp->sleep_avg * 4) {
        dp->bonus = -5;  /* CPU 密集型任务降低优先级 */
    } else if (dp->run_avg > dp->sleep_avg * 2) {
        dp->bonus = -2;
    } else {
        dp->bonus = 0;
    }
}

/* 更新运行时间 */
void cfs_update_run(struct sched_entity *se, uint64_t run_time) {
    if (se == NULL || se->thread == NULL) return;
    
    uint32_t tid = se->thread->tid;
    struct dynamic_prio_data *dp = &dynamic_prio[tid % MAX_THREADS];
    
    dp->run_avg = (dp->run_avg * DYNAMIC_PRIO_DECAY + run_time) / 
                  (DYNAMIC_PRIO_DECAY + 1);
    dp->last_run = run_time;
}

/* 获取动态优先级奖励 */
int cfs_get_dynamic_bonus(struct sched_entity *se) {
    if (se == NULL || se->thread == NULL) return 0;
    return dynamic_prio[se->thread->tid % MAX_THREADS].bonus;
}

/* === CFS 自动分组 === */

#define AUTO_GROUP_MAX          32
#define AUTO_GROUP_WINDOW       100000000ULL  /* 100ms */

struct auto_group {
    uint32_t id;
    char name[32];
    uint64_t shares;
    uint64_t last_activity;
    int member_count;
    int active;
};

static struct auto_group auto_groups[AUTO_GROUP_MAX];
static uint32_t next_auto_group = 1;

/* 检测是否应该创建自动分组 */
int cfs_check_auto_group(struct sched_entity *se) {
    if (se == NULL || se->thread == NULL) return -1;
    
    uint32_t tid = se->thread->tid;
    struct dynamic_prio_data *dp = &dynamic_prio[tid % MAX_THREADS];
    
    /* 交互式任务自动分组 */
    if (dp->interactive && dp->bonus >= 4) {
        return 1;  /* 建议分到交互组 */
    }
    
    /* CPU 密集型任务自动分组 */
    if (dp->bonus <= -4) {
        return 2;  /* 建议分到批处理组 */
    }
    
    return 0;  /* 不分组 */
}

/* 创建自动分组 */
int cfs_create_auto_group(int type) {
    if (next_auto_group >= AUTO_GROUP_MAX) {
        return -1;
    }
    
    int idx = next_auto_group++;
    auto_groups[idx].id = idx;
    auto_groups[idx].active = 1;
    auto_groups[idx].member_count = 0;
    auto_groups[idx].last_activity = cfs_get_time();
    
    switch (type) {
        case 1:  /* 交互组 */
            auto_groups[idx].name[0] = 'i';
            auto_groups[idx].name[1] = 'n';
            auto_groups[idx].name[2] = 't';
            auto_groups[idx].name[3] = '\0';
            auto_groups[idx].shares = 2048;  /* 更高份额 */
            break;
        case 2:  /* 批处理组 */
            auto_groups[idx].name[0] = 'b';
            auto_groups[idx].name[1] = 'a';
            auto_groups[idx].name[2] = 't';
            auto_groups[idx].name[3] = 'c';
            auto_groups[idx].name[4] = 'h';
            auto_groups[idx].name[5] = '\0';
            auto_groups[idx].shares = 512;  /* 较低份额 */
            break;
        default:
            auto_groups[idx].shares = 1024;
            break;
    }
    
    return idx;
}

/* === CFS 抢占优化 === */

/* 检查是否应该抢占当前任务 */
int cfs_should_preempt(struct cfs_rq *cfs_rq, struct sched_entity *curr, 
                       struct sched_entity *se) {
    if (cfs_rq == NULL || curr == NULL || se == NULL) {
        return 0;
    }
    
    /* 当前任务正在运行 */
    uint64_t curr_run_time = curr->sum_exec_runtime - curr->prev_sum_exec_runtime;
    uint64_t quarter_slice = curr->slice / 4;
    
    if (curr_run_time < quarter_slice) {
        /* 使用了不到 1/4 时间片，检查是否值得抢占 */
        
        /* 交互式任务优先 */
        int curr_bonus = cfs_get_dynamic_bonus(curr);
        int se_bonus = cfs_get_dynamic_bonus(se);
        
        if (se_bonus > curr_bonus + 3) {
            return 1;  /* 高优先级交互任务抢占 */
        }
        
        /* 检查虚拟时间差距 */
        uint64_t vruntime_diff = se->vruntime - curr->vruntime;
        if (vruntime_diff > cfs_rq->sched_latency) {
            return 0;  /* 新任务虚拟时间更大，不抢占 */
        }
        
        /* 检查唤醒的任务 */
        if (se->thread && (se->thread->flags & 0x01)) {  /* 假设 0x01 是唤醒标志 */
            return 1;
        }
    }
    
    /* 使用超过一半时间片，可以抢占 */
    if (curr_run_time >= curr->slice / 2) {
        return 1;
    }
    
    return 0;
}

/* 抢占检查粒度 */
#define PREEMPT_GRANULARITY     1000000ULL  /* 1ms */

/* 更新抢占点 - 简化版 */
void cfs_update_preempt_point(struct cfs_rq *cfs_rq, struct sched_entity *se) {
    if (cfs_rq == NULL || se == NULL) return;
    
    /* 简化实现：设置线程的标志 */
    /* 实际实现需要在 tick 中检查 */
}

/* === CFS 睡眠追踪 === */

#define SLEEP_BUCKET_COUNT      16
#define SLEEP_HISTORY_SIZE      64

struct sleep_bucket {
    uint64_t min_sleep;
    uint64_t max_sleep;
    uint32_t count;
};

struct sleep_tracker {
    struct sleep_bucket buckets[SLEEP_BUCKET_COUNT];
    uint64_t history[SLEEP_HISTORY_SIZE];
    uint32_t history_head;
    uint32_t total_samples;
    uint64_t avg_sleep;
    uint64_t std_dev;
};

static struct sleep_tracker sleep_trackers[MAX_THREADS];

/* 记录睡眠时间 */
void cfs_record_sleep(uint32_t tid, uint64_t sleep_time) {
    if (tid >= MAX_THREADS) return;
    
    struct sleep_tracker *st = &sleep_trackers[tid];
    
    /* 添加到历史 */
    st->history[st->history_head++ % SLEEP_HISTORY_SIZE] = sleep_time;
    st->total_samples++;
    
    /* 更新平均 */
    st->avg_sleep = (st->avg_sleep * 7 + sleep_time) / 8;
    
    /* 计算标准差 */
    uint64_t variance = 0;
    uint32_t samples = st->total_samples < SLEEP_HISTORY_SIZE ? 
                       st->total_samples : SLEEP_HISTORY_SIZE;
    for (uint32_t i = 0; i < samples; i++) {
        int64_t diff = (int64_t)st->history[i] - (int64_t)st->avg_sleep;
        variance += diff * diff;
    }
    st->std_dev = variance / samples;
    
    /* 更新桶 */
    int bucket = 0;
    uint64_t threshold = 1000000ULL;  /* 1ms */
    while (bucket < SLEEP_BUCKET_COUNT - 1 && sleep_time > threshold) {
        threshold *= 2;
        bucket++;
    }
    
    if (st->buckets[bucket].count == 0) {
        st->buckets[bucket].min_sleep = sleep_time;
        st->buckets[bucket].max_sleep = sleep_time;
    } else {
        if (sleep_time < st->buckets[bucket].min_sleep) {
            st->buckets[bucket].min_sleep = sleep_time;
        }
        if (sleep_time > st->buckets[bucket].max_sleep) {
            st->buckets[bucket].max_sleep = sleep_time;
        }
    }
    st->buckets[bucket].count++;
}

/* 预测下次睡眠时间 */
uint64_t cfs_predict_sleep(uint32_t tid) {
    if (tid >= MAX_THREADS) return 0;
    
    struct sleep_tracker *st = &sleep_trackers[tid];
    
    if (st->total_samples == 0) return 0;
    
    /* 使用指数移动平均作为预测 */
    return st->avg_sleep;
}

/* === CFS 调度延迟分析 === */

struct latency_stats {
    uint64_t min_latency;        /* 最小调度延迟 */
    uint64_t max_latency;        /* 最大调度延迟 */
    uint64_t avg_latency;        /* 平均调度延迟 */
    uint64_t total_wait_time;    /* 总等待时间 */
    uint64_t samples;            /* 样本数 */
    uint64_t p50;                /* 50 百分位 */
    uint64_t p95;                /* 95 百分位 */
    uint64_t p99;                /* 99 百分位 */
};

static struct latency_stats latency_stats_per_cpu[MAX_CPUS];
static volatile int latency_lock = 0;

/* 记录调度延迟 */
void cfs_record_latency(int cpu, uint64_t wait_time) {
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    
    while (__sync_lock_test_and_set(&latency_lock, 1)) {
        asm volatile("pause");
    }
    
    struct latency_stats *ls = &latency_stats_per_cpu[cpu];
    
    if (ls->samples == 0) {
        ls->min_latency = wait_time;
        ls->max_latency = wait_time;
        ls->avg_latency = wait_time;
    } else {
        if (wait_time < ls->min_latency) ls->min_latency = wait_time;
        if (wait_time > ls->max_latency) ls->max_latency = wait_time;
        
        /* 指数移动平均 */
        ls->avg_latency = (ls->avg_latency * 7 + wait_time) / 8;
    }
    
    ls->total_wait_time += wait_time;
    ls->samples++;
    
    /* 简化的百分位数估计 */
    if (wait_time >= ls->p95) {
        ls->p99 = (ls->p99 * 15 + wait_time) / 16;
    }
    if (wait_time >= ls->avg_latency) {
        ls->p95 = (ls->p95 * 15 + wait_time) / 16;
    }
    ls->p50 = ls->avg_latency;  /* 简化 */
    
    __sync_lock_release(&latency_lock);
}

/* 获取调度延迟统计 */
void cfs_get_latency_stats(int cpu, struct latency_stats *stats) {
    if (cpu < 0 || cpu >= MAX_CPUS || stats == NULL) return;
    
    while (__sync_lock_test_and_set(&latency_lock, 1)) {
        asm volatile("pause");
    }
    
    *stats = latency_stats_per_cpu[cpu];
    
    __sync_lock_release(&latency_lock);
}

/* === CFS 综合诊断 === */

void cfs_print_comprehensive_report(void) {
    println("");
    println("========================================");
    println("    CFS Comprehensive Report          ");
    println("========================================");
    println("");
    
    /* 基本配置 */
    println("--- Configuration ---");
    print("Sched Latency: ");
    print_dec(CFS_SCHED_LATENCY / 1000000);
    println(" ms");
    
    print("Min Granularity: ");
    print_dec(CFS_MIN_GRANULARITY / 1000000);
    println(" ms");
    
    print("Min Slice: ");
    print_dec(CFS_MIN_SLICE / 1000000);
    println(" ms");
    
    print("Max Slice: ");
    print_dec(CFS_MAX_SLICE / 1000000);
    println(" ms");
    
    /* Per-CPU 状态 */
    println("");
    println("--- Per-CPU Status ---");
    
    for (int i = 0; i < MAX_CPUS; i++) {
        struct cfs_rq *rq = &cfs_rqs[i];
        
        if (rq->nr_running == 0) continue;
        
        print("CPU ");
        print_dec(i);
        print(": ");
        print_dec(rq->nr_running);
        print(" tasks, load=");
        print_dec(rq->load_weight);
        print(", min_vruntime=");
        print_dec(rq->min_vruntime);
        println("");
    }
    
    /* 性能统计 */
    println("");
    println("--- Performance Stats ---");
    
    struct cfs_stats stats;
    cfs_get_stats(&stats);
    
    print("Context Switches: ");
    print_dec(stats.context_switches);
    println("");
    
    print("Avg Slice: ");
    print_dec(stats.avg_slice / 1000000);
    println(" ms");
    
    print("Max Slice: ");
    print_dec(stats.max_slice / 1000000);
    println(" ms");
    
    print("Min Slice: ");
    print_dec(stats.min_slice / 1000000);
    println(" ms");
    
    /* 调度延迟统计 */
    println("");
    println("--- Scheduling Latency ---");
    
    for (int i = 0; i < MAX_CPUS; i++) {
        struct latency_stats ls;
        cfs_get_latency_stats(i, &ls);
        
        if (ls.samples == 0) continue;
        
        print("CPU ");
        print_dec(i);
        print(": avg=");
        print_dec(ls.avg_latency / 1000000);
        print(" ms, max=");
        print_dec(ls.max_latency / 1000000);
        println(" ms");
    }
    
    /* 带宽控制状态 */
    println("");
    println("--- Bandwidth Control ---");
    print("Global Quota: ");
    print_dec(global_bandwidth.quota / 1000000);
    println(" ms");
    print("Global Period: ");
    print_dec(global_bandwidth.period / 1000000);
    println(" ms");
    print("Remaining Runtime: ");
    print_dec(global_bandwidth.runtime / 1000000);
    println(" ms");
    
    println("");
    println("========================================");
    println("          End of Report");
    println("========================================");
    println("");
}

/* === CFS 增强初始化 === */

void cfs_init_enhanced(void) {
    /* 初始化基础 CFS */
    cfs_init();
    
    /* 初始化动态优先级数据 */
    for (int i = 0; i < MAX_THREADS; i++) {
        dynamic_prio[i].sleep_avg = 0;
        dynamic_prio[i].run_avg = 0;
        dynamic_prio[i].bonus = 0;
        dynamic_prio[i].interactive = 0;
    }
    
    /* 初始化自动分组 */
    for (int i = 0; i < AUTO_GROUP_MAX; i++) {
        auto_groups[i].active = 0;
    }
    
    /* 初始化睡眠追踪器 */
    for (int i = 0; i < MAX_THREADS; i++) {
        sleep_trackers[i].total_samples = 0;
        sleep_trackers[i].history_head = 0;
        sleep_trackers[i].avg_sleep = 0;
        sleep_trackers[i].std_dev = 0;
    }
    
    /* 初始化延迟统计 */
    for (int i = 0; i < MAX_CPUS; i++) {
        latency_stats_per_cpu[i].samples = 0;
        latency_stats_per_cpu[i].min_latency = ~0ULL;
        latency_stats_per_cpu[i].max_latency = 0;
        latency_stats_per_cpu[i].avg_latency = 0;
        latency_stats_per_cpu[i].total_wait_time = 0;
    }
    
    println("[CFS] Enhanced CFS initialized");
}

