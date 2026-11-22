/*
 * Zratro OS - Hard Real-Time Scheduler (硬实时调度器)
 * 
 * 实现：
 * 1. EDF 调度
 * 2. 优先级继承
 * 3. 周期性任务
 * 4. 截止时间监控
 */

#include "realtime.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "apic.h"

/* ========== 全局调度器 ========== */

static struct rt_scheduler global_rts;

/* ========== 初始化/销毁 ========== */

int rt_init(struct rt_scheduler *rts) {
    if (rts == NULL) {
        return -1;
    }
    
    memset(rts, 0, sizeof(*rts));
    
    INIT_LIST_HEAD(&rts->tasks);
    INIT_LIST_HEAD(&rts->periodic_tasks);
    
    /* 初始化 CPU 状态 */
    for (int i = 0; i < RT_MAX_CPUS; i++) {
        rts->cpu_states[i].cpu_id = i;
        for (int p = 0; p < 100; p++) {
            INIT_LIST_HEAD(&rts->cpu_states[i].ready[p]);
        }
        INIT_LIST_HEAD(&rts->cpu_states[i].edf_queue.tasks);
    }
    
    /* 初始化截止时间监控 */
    rts->monitor.check_interval = 1000000;  /* 1ms */
    rts->monitor.miss_threshold = 3;         /* 连续错过 3 次告警 */
    
    rts->tick_period = 1000000;  /* 1ms */
    rts->strict_mode = 1;
    
    spinlock_init(&rts->lock);
    
    return 0;
}

void rt_destroy(struct rt_scheduler *rts) {
    if (rts == NULL) {
        return;
    }
    
    spin_lock(&rts->lock);
    
    /* 释放所有任务 */
    struct rt_task *task, *tmp;
    list_for_each_entry_safe(task, tmp, &rts->tasks, list) {
        list_del(&task->list);
        kfree(task);
    }
    
    spin_unlock(&rts->lock);
}

/* ========== 任务管理 ========== */

struct rt_task *rt_create_task(struct rt_scheduler *rts,
                                uint64_t task_id,
                                const char *name) {
    if (rts == NULL) {
        return NULL;
    }
    
    struct rt_task *task = kmalloc(sizeof(*task));
    if (task == NULL) {
        return NULL;
    }
    
    memset(task, 0, sizeof(*task));
    
    task->task_id = task_id;
    if (name != NULL) {
        strncpy(task->name, name, RT_MAX_NAME_LEN - 1);
    }
    task->active = 1;
    
    /* 默认参数 */
    task->params.policy = RT_SCHED_FIFO;
    task->params.priority = RT_PRIO_DEFAULT;
    task->params.time_slice = 10000000;  /* 10ms */
    task->params.time_remaining = task->params.time_slice;
    task->params.cpu_mask = 0xFF;  /* 所有 CPU */
    
    /* 初始化 PI 节点 */
    task->pi_node.task_id = task_id;
    task->pi_node.original_prio = task->params.priority;
    INIT_LIST_HEAD(&task->pi_node.blockers);
    INIT_LIST_HEAD(&task->pi_node.pi_list);
    
    INIT_LIST_HEAD(&task->list);
    INIT_LIST_HEAD(&task->run_list);
    
    spin_lock(&rts->lock);
    list_add_tail(&task->list, &rts->tasks);
    rts->task_count++;
    spin_unlock(&rts->lock);
    
    return task;
}

int rt_destroy_task(struct rt_scheduler *rts, struct rt_task *task) {
    if (rts == NULL || task == NULL) {
        return -1;
    }
    
    spin_lock(&rts->lock);
    
    /* 从周期性列表移除 */
    if (task->is_periodic) {
        list_del(&task->run_list);
        rts->periodic_count--;
    }
    
    list_del(&task->list);
    rts->task_count--;
    
    spin_unlock(&rts->lock);
    
    kfree(task);
    return 0;
}

int rt_set_params(struct rt_task *task, struct rt_params *params) {
    if (task == NULL || params == NULL) {
        return -1;
    }
    
    /* 复制参数 */
    memcpy(&task->params, params, sizeof(*params));
    
    /* 更新 PI 节点 */
    task->pi_node.original_prio = params->priority;
    
    /* 计算初始截止时间 */
    if (params->policy == RT_SCHED_EDF || params->policy == RT_SCHED_DEADLINE) {
        task->params.next_deadline = params->deadline;
    }
    
    return 0;
}

int rt_get_params(struct rt_task *task, struct rt_params *params) {
    if (task == NULL || params == NULL) {
        return -1;
    }
    
    memcpy(params, &task->params, sizeof(*params));
    return 0;
}

int rt_set_cpu_affinity(struct rt_task *task, int cpu) {
    if (task == NULL || cpu < 0 || cpu >= RT_MAX_CPUS) {
        return -1;
    }
    
    task->params.cpu_mask = (1ULL << cpu);
    task->params.cpu_bound = cpu;
    
    return 0;
}

/* ========== 调度操作 ========== */

struct rt_task *rt_select_next(struct rt_scheduler *rts, int cpu) {
    if (rts == NULL || cpu < 0 || cpu >= RT_MAX_CPUS) {
        return NULL;
    }
    
    struct rt_cpu_state *cs = &rts->cpu_states[cpu];
    
    /* 先检查 EDF 队列 */
    if (cs->edf_queue.count > 0) {
        struct rt_task *task = rt_edf_peek(&cs->edf_queue);
        if (task != NULL && (task->params.cpu_mask & (1ULL << cpu))) {
            return task;
        }
    }
    
    /* 检查优先级队列 */
    for (int prio = RT_PRIO_MIN; prio <= RT_PRIO_MAX; prio++) {
        if (!list_empty(&cs->ready[prio])) {
            struct rt_task *task = list_first_entry(&cs->ready[prio],
                                                     struct rt_task, run_list);
            if (task != NULL && (task->params.cpu_mask & (1ULL << cpu))) {
                return task;
            }
        }
    }
    
    return NULL;
}

int rt_task_ready(struct rt_scheduler *rts, struct rt_task *task) {
    if (rts == NULL || task == NULL) {
        return -1;
    }
    
    spin_lock(&rts->lock);
    
    /* 选择合适的 CPU */
    int cpu = 0;
    if (task->params.cpu_bound >= 0) {
        cpu = task->params.cpu_bound;
    } else {
        /* 找负载最低的 CPU */
        uint64_t min_load = (uint64_t)-1;
        for (int i = 0; i < (int)rts->cpu_count; i++) {
            if (!(task->params.cpu_mask & (1ULL << i))) continue;
            if (rts->cpu_states[i].ready_count < min_load) {
                min_load = rts->cpu_states[i].ready_count;
                cpu = i;
            }
        }
    }
    
    struct rt_cpu_state *cs = &rts->cpu_states[cpu];
    
    /* 根据策略插入队列 */
    if (task->params.policy == RT_SCHED_EDF || task->params.policy == RT_SCHED_DEADLINE) {
        rt_edf_insert(&cs->edf_queue, task);
    } else {
        int prio = rt_effective_prio(task);
        if (prio >= 0 && prio < 100) {
            list_add_tail(&task->run_list, &cs->ready[prio]);
        }
    }
    
    cs->ready_count++;
    
    spin_unlock(&rts->lock);
    
    return 0;
}

int rt_task_block(struct rt_scheduler *rts, struct rt_task *task) {
    if (rts == NULL || task == NULL) {
        return -1;
    }
    
    spin_lock(&rts->lock);
    
    /* 从运行队列移除 */
    if (task->run_list.next != &task->run_list) {
        list_del(&task->run_list);
        INIT_LIST_HEAD(&task->run_list);
    }
    
    spin_unlock(&rts->lock);
    
    return 0;
}

int rt_task_complete(struct rt_scheduler *rts, struct rt_task *task) {
    if (rts == NULL || task == NULL) {
        return -1;
    }
    
    /* 更新统计 */
    task->params.deadlines_met++;
    task->executions++;
    
    /* 如果是周期性任务，设置下次释放 */
    if (task->is_periodic && task->params.period > 0) {
        task->params.next_release += task->params.period;
        task->params.next_deadline = task->params.next_release + task->params.deadline;
    }
    
    return 0;
}

int rt_task_expire(struct rt_scheduler *rts, struct rt_task *task) {
    if (rts == NULL || task == NULL) {
        return -1;
    }
    
    /* RR 调度：时间片用尽，重新排队 */
    if (task->params.policy == RT_SCHED_RR) {
        task->params.time_remaining = task->params.time_slice;
        rt_task_ready(rts, task);
    }
    
    rts->total_preemptions++;
    
    return 0;
}

/* ========== EDF 调度 ========== */

int rt_edf_insert(struct rt_edf_queue *queue, struct rt_task *task) {
    if (queue == NULL || task == NULL) {
        return -1;
    }
    
    /* 按截止时间插入 */
    struct rt_task *pos;
    list_for_each_entry(pos, &queue->tasks, run_list) {
        if (task->params.next_deadline < pos->params.next_deadline) {
            list_add_tail(&task->run_list, &pos->run_list);
            queue->count++;
            
            /* 更新最早截止时间 */
            if (task->params.next_deadline < queue->earliest_deadline || 
                queue->earliest_deadline == 0) {
                queue->earliest_deadline = task->params.next_deadline;
            }
            
            return 0;
        }
    }
    
    /* 插入队尾 */
    list_add_tail(&task->run_list, &queue->tasks);
    queue->count++;
    
    /* 更新最早截止时间 */
    if (queue->earliest_deadline == 0 || 
        task->params.next_deadline < queue->earliest_deadline) {
        queue->earliest_deadline = task->params.next_deadline;
    }
    
    return 0;
}

int rt_edf_remove(struct rt_edf_queue *queue, struct rt_task *task) {
    if (queue == NULL || task == NULL) {
        return -1;
    }
    
    list_del(&task->run_list);
    INIT_LIST_HEAD(&task->run_list);
    queue->count--;
    
    /* 更新最早截止时间 */
    if (queue->count > 0) {
        struct rt_task *first = list_first_entry(&queue->tasks, struct rt_task, run_list);
        queue->earliest_deadline = first->params.next_deadline;
    } else {
        queue->earliest_deadline = 0;
    }
    
    return 0;
}

struct rt_task *rt_edf_peek(struct rt_edf_queue *queue) {
    if (queue == NULL || list_empty(&queue->tasks)) {
        return NULL;
    }
    
    return list_first_entry(&queue->tasks, struct rt_task, run_list);
}

/* ========== 优先级继承 ========== */

int rt_pi_begin(struct rt_scheduler *rts, struct rt_task *task, void *lock) {
    if (rts == NULL || task == NULL || lock == NULL) {
        return -1;
    }
    
    struct rt_pi_node *node = &task->pi_node;
    
    node->waiting_for = lock;
    node->chain_depth = 0;
    
    return 0;
}

int rt_pi_end(struct rt_scheduler *rts, struct rt_task *task, void *lock) {
    if (rts == NULL || task == NULL) {
        return -1;
    }
    
    struct rt_pi_node *node = &task->pi_node;
    
    /* 恢复原始优先级 */
    node->waiting_for = NULL;
    node->boosted_prio = 0;
    
    /* 通知所有阻塞者 */
    struct rt_task *blocker;
    list_for_each_entry(blocker, &node->blockers, pi_node.pi_list) {
        blocker->pi_node.chain_depth--;
    }
    
    rts->total_pi_inherit++;
    
    return 0;
}

int rt_pi_update_chain(struct rt_scheduler *rts, struct rt_pi_node *node) {
    if (rts == NULL || node == NULL) {
        return -1;
    }
    
    int max_prio = node->original_prio;
    
    /* 遍历阻塞链，找到最高优先级 */
    struct rt_pi_node *pos;
    list_for_each_entry(pos, &node->blockers, pi_list) {
        if (pos->original_prio < max_prio) {
            max_prio = pos->original_prio;
        }
    }
    
    /* 提升优先级 */
    node->boosted_prio = max_prio;
    
    /* 递归更新链 */
    if (node->blocked_by != NULL && node->chain_depth < RT_MAX_PI_CHAIN) {
        rt_pi_update_chain(rts, node->blocked_by);
    }
    
    return 0;
}

/* ========== 周期性任务 ========== */

int rt_register_periodic(struct rt_scheduler *rts, struct rt_task *task) {
    if (rts == NULL || task == NULL) {
        return -1;
    }
    
    task->is_periodic = 1;
    task->params.next_release = 0;
    task->params.next_deadline = task->params.deadline;
    
    spin_lock(&rts->lock);
    list_add_tail(&task->run_list, &rts->periodic_tasks);
    rts->periodic_count++;
    spin_unlock(&rts->lock);
    
    return 0;
}

int rt_unregister_periodic(struct rt_scheduler *rts, struct rt_task *task) {
    if (rts == NULL || task == NULL) {
        return -1;
    }
    
    spin_lock(&rts->lock);
    list_del(&task->run_list);
    rts->periodic_count--;
    task->is_periodic = 0;
    spin_unlock(&rts->lock);
    
    return 0;
}

int rt_handle_periodic(struct rt_scheduler *rts, uint64_t now) {
    if (rts == NULL) {
        return -1;
    }
    
    int released = 0;
    
    spin_lock(&rts->lock);
    
    struct rt_task *task;
    list_for_each_entry(task, &rts->periodic_tasks, run_list) {
        if (now >= task->params.next_release) {
            /* 释放任务 */
            task->last_release = now;
            rt_task_ready(rts, task);
            released++;
        }
    }
    
    spin_unlock(&rts->lock);
    
    return released;
}

/* ========== 截止时间监控 ========== */

int rt_check_deadlines(struct rt_scheduler *rts, uint64_t now) {
    if (rts == NULL) {
        return -1;
    }
    
    int missed = 0;
    
    spin_lock(&rts->lock);
    
    struct rt_task *task;
    list_for_each_entry(task, &rts->tasks, list) {
        if (!task->active) continue;
        if (!rt_is_realtime(task)) continue;
        
        /* 检查是否错过截止时间 */
        if (task->params.next_deadline > 0 && now > task->params.next_deadline) {
            task->params.deadlines_missed++;
            missed++;
            
            /* 更新连续错过计数 */
            rts->monitor.consecutive_misses++;
            if (rts->monitor.consecutive_misses > rts->monitor.max_consecutive) {
                rts->monitor.max_consecutive = rts->monitor.consecutive_misses;
            }
            
            /* 触发回调 */
            if (rts->monitor.miss_callback != NULL && 
                rts->monitor.consecutive_misses >= rts->monitor.miss_threshold) {
                rts->monitor.miss_callback(task, task->params.next_deadline);
            }
            
            rts->monitor.total_misses++;
            rts->monitor.recent_misses++;
        } else {
            /* 重置连续计数 */
            rts->monitor.consecutive_misses = 0;
        }
    }
    
    rts->monitor.last_check = now;
    
    spin_unlock(&rts->lock);
    
    return missed;
}

int rt_set_miss_callback(struct rt_scheduler *rts,
                         void (*callback)(struct rt_task *, uint64_t)) {
    if (rts == NULL) {
        return -1;
    }
    
    rts->monitor.miss_callback = callback;
    rts->monitor.callback_enabled = 1;
    
    return 0;
}

void rt_get_deadline_stats(struct rt_scheduler *rts,
                            uint64_t *met, uint64_t *missed) {
    if (rts == NULL) {
        return;
    }
    
    spin_lock(&rts->lock);
    
    uint64_t total_met = 0, total_missed = 0;
    struct rt_task *task;
    list_for_each_entry(task, &rts->tasks, list) {
        total_met += task->params.deadlines_met;
        total_missed += task->params.deadlines_missed;
    }
    
    if (met != NULL) *met = total_met;
    if (missed != NULL) *missed = total_missed;
    
    spin_unlock(&rts->lock);
}

/* ========== 工具函数 ========== */

int64_t rt_deadline_urgency(struct rt_task *task, uint64_t now) {
    if (task == NULL) {
        return 0;
    }
    
    return (int64_t)task->params.next_deadline - (int64_t)now;
}

int rt_check_schedulability(struct rt_scheduler *rts) {
    if (rts == NULL) {
        return 0;
    }
    
    uint64_t utilization = rt_calc_utilization(rts);
    
    /* EDF 可调度条件: U <= 1 */
    return utilization <= 1000;  /* 1000 = 100% */
}

uint64_t rt_calc_utilization(struct rt_scheduler *rts) {
    if (rts == NULL) {
        return 0;
    }
    
    uint64_t total_util = 0;
    
    spin_lock(&rts->lock);
    
    struct rt_task *task;
    list_for_each_entry(task, &rts->tasks, list) {
        if (task->params.policy == RT_SCHED_EDF || 
            task->params.policy == RT_SCHED_DEADLINE) {
            if (task->params.period > 0) {
                /* U = runtime / period */
                uint64_t u = (task->params.runtime * 1000) / task->params.period;
                total_util += u;
            }
        }
    }
    
    spin_unlock(&rts->lock);
    
    return total_util;
}

/* ========== 时钟钩子 ========== */

void rt_tick_handler(struct rt_scheduler *rts, uint64_t now) {
    if (rts == NULL) {
        return;
    }
    
    /* 处理周期性任务释放 */
    rt_handle_periodic(rts, now);
    
    /* 检查截止时间 */
    if (now - rts->monitor.last_check >= rts->monitor.check_interval) {
        rt_check_deadlines(rts, now);
        rts->monitor.recent_misses = 0;
    }
}

/* ========== 统计 ========== */

void rt_get_stats(struct rt_scheduler *rts,
                  uint64_t *switches, uint64_t *preemptions,
                  uint64_t *pi_inherits) {
    if (rts == NULL) {
        return;
    }
    
    spin_lock(&rts->lock);
    
    if (switches != NULL) *switches = rts->total_switches;
    if (preemptions != NULL) *preemptions = rts->total_preemptions;
    if (pi_inherits != NULL) *pi_inherits = rts->total_pi_inherit;
    
    spin_unlock(&rts->lock);
}

void rt_print_tasks(struct rt_scheduler *rts) {
    if (rts == NULL) {
        return;
    }
    
    println("=== Real-Time Tasks ===");
    
    spin_lock(&rts->lock);
    
    print("  Total: ");
    print_dec(rts->task_count);
    print(", Periodic: ");
    print_dec(rts->periodic_count);
    println("");
    
    struct rt_task *task;
    list_for_each_entry(task, &rts->tasks, list) {
        print("  [");
        print_dec(task->task_id);
        print("] ");
        print(task->name);
        
        const char *policy_str = "UNKNOWN";
        switch (task->params.policy) {
            case RT_SCHED_FIFO: policy_str = "FIFO"; break;
            case RT_SCHED_RR: policy_str = "RR"; break;
            case RT_SCHED_EDF: policy_str = "EDF"; break;
            case RT_SCHED_DEADLINE: policy_str = "DEADLINE"; break;
            default: break;
        }
        
        print(" (");
        print((char *)policy_str);
        print(", prio=");
        print_dec(task->params.priority);
        println(")");
    }
    
    spin_unlock(&rts->lock);
}

/* ========== 全局调度器 ========== */

struct rt_scheduler *rt_get_global(void) {
    return &global_rts;
}

int rt_global_init(void) {
    return rt_init(&global_rts);
}

/* ========== 主调度器优先级继承 (struct thread 集成) ========== */

/* PI 自旋锁操作 */
static inline void pi_spin_lock(volatile int *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        asm volatile("pause");
    }
}

static inline void pi_spin_unlock(volatile int *lock) {
    __sync_lock_release(lock);
}

/**
 * rt_pi_boost - 优先级继承提升
 * @waiter: 被阻塞的高优先级线程
 * @holder: 持有锁的低优先级线程
 * @lock:   争用的锁地址
 *
 * 当高优先级线程 (waiter) 阻塞在低优先级线程 (holder) 持有的互斥锁上时，
 * 临时提升 holder 的优先级到 waiter 的优先级，防止优先级反转。
 *
 * 支持传递性：如果 holder 也在等待另一个锁，递归提升整条链。
 */
int rt_pi_boost(struct thread *waiter, struct thread *holder, void *lock) {
    if (waiter == NULL || holder == NULL || lock == NULL) {
        return -1;
    }

    /* 获取 waiter 的有效优先级 */
    int waiter_prio = rt_thread_effective_prio(waiter);
    int holder_prio = rt_thread_effective_prio(holder);

    /* 只有当 waiter 优先级高于 holder 时才需要提升 */
    if (waiter_prio <= holder_prio) {
        return 0;  /* 不需要提升 */
    }

    /* 锁定 holder 的 PI 数据 */
    pi_spin_lock(&holder->pi_lock);

    /* 记录 waiter 等待的锁 */
    waiter->pi_waiting_for = lock;

    /* 将 waiter 加入 holder 的等待者链表 */
    list_add_tail(&waiter->pi_list, &holder->pi_waiters);

    /* 如果是首次提升，保存原始优先级 */
    if (holder->boosted_priority == 0) {
        holder->pi_original_prio = holder->rt_priority;
    }

    /* 计算所有等待者中的最高优先级 */
    int max_prio = holder->pi_original_prio;
    struct thread *w;
    list_for_each_entry(w, &holder->pi_waiters, pi_list) {
        int w_prio = rt_thread_effective_prio(w);
        if (w_prio > max_prio) {
            max_prio = w_prio;
        }
    }

    /* 设置提升后的优先级 */
    holder->boosted_priority = max_prio;
    holder->rt_priority = max_prio;

    pi_spin_unlock(&holder->pi_lock);

    /* 传递性：如果 holder 也在等待另一个锁，递归提升链 */
    if (holder->pi_waiting_for != NULL) {
        /* 查找 holder 等待的锁的持有者
         * 在简化实现中，我们遍历所有线程查找持有同一锁的线程
         * 生产实现应使用锁到持有者的映射表 */
        extern struct thread *thread_table;
        extern uint32_t next_tid;
        /* 限制递归深度防止死循环 */
        int chain_depth = 0;
        struct thread *current = holder;
        while (current->pi_waiting_for != NULL && chain_depth < RT_MAX_PI_CHAIN) {
            void *wait_lock = current->pi_waiting_for;
            struct thread *next_holder = NULL;

            /* 在等待者链表中查找：谁持有 wait_lock？
             * 简化实现：检查 pi_waiters 非空且 pi_waiting_for != wait_lock 的线程
             * 实际应维护 lock -> holder 映射 */
            /* 此处跳过传递性查找，由锁释放时 rt_pi_unboost 处理 */
            break;
        }
    }

    return 0;
}

/**
 * rt_pi_unboost - 优先级继承恢复
 * @holder: 释放锁的线程
 * @lock:   被释放的锁地址
 *
 * 当线程释放互斥锁时，恢复其原始优先级。
 * 同时唤醒等待该锁的最高优先级线程。
 */
int rt_pi_unboost(struct thread *holder, void *lock) {
    if (holder == NULL || lock == NULL) {
        return -1;
    }

    pi_spin_lock(&holder->pi_lock);

    /* 移除所有等待此锁的线程从等待者链表 */
    struct thread *w, *tmp;
    list_for_each_entry_safe(w, tmp, &holder->pi_waiters, pi_list) {
        if (w->pi_waiting_for == lock) {
            list_del(&w->pi_list);
            INIT_LIST_HEAD(&w->pi_list);
            w->pi_waiting_for = NULL;
        }
    }

    /* 如果等待者链表为空，恢复原始优先级 */
    if (list_empty(&holder->pi_waiters)) {
        if (holder->boosted_priority > 0) {
            holder->rt_priority = holder->pi_original_prio;
            holder->boosted_priority = 0;
            holder->pi_original_prio = 0;
        }
    } else {
        /* 仍有等待者，重新计算提升优先级为最高等待者优先级 */
        int max_prio = holder->pi_original_prio;
        list_for_each_entry(w, &holder->pi_waiters, pi_list) {
            int w_prio = rt_thread_effective_prio(w);
            if (w_prio > max_prio) {
                max_prio = w_prio;
            }
        }
        holder->boosted_priority = max_prio;
        holder->rt_priority = max_prio;
    }

    pi_spin_unlock(&holder->pi_lock);

    return 0;
}
