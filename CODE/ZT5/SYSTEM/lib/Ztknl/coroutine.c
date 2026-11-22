/*
 * Zratro OS - Coroutine (协程原生调度支持)
 * 
 * 实现：
 * 1. 协程创建与销毁
 * 2. 上下文切换 (汇编优化)
 * 3. 调度器集成
 * 4. 协程间同步
 */

#include "coroutine.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "task.h"
#include "scheduler.h"

/* ========== 全局状态 ========== */

static uint64_t next_co_id = 1;
static struct list_head all_coroutines;
static spinlock_t global_lock;

/* 线程本地调度器 (每个线程一个) */
#define MAX_CO_SCHEDULERS 64
static struct co_scheduler co_schedulers[MAX_CO_SCHEDULERS];

/* ========== 汇编接口 (context_asm.S) ========== */

extern void co_switch(struct co_context *from, struct co_context *to);
extern void co_entry_wrapper(void);

/* ========== 协程上下文初始化 ========== */

static void co_context_init(struct coroutine *co) {
    /* 设置栈顶 (向下生长，留出安全边界) */
    uint64_t stack_top = (uint64_t)co->stack_base + co->stack_size - 16;

    /* 对齐到 16 字节 */
    stack_top &= ~0xFULL;

    /* 设置初始上下文 */
    co->context.rsp = stack_top;
    co->context.rbp = stack_top;
    co->context.rip = (uint64_t)co_entry_wrapper;

    /* 在栈上放置入口参数 */
    stack_top -= 16;
    *(uint64_t *)stack_top = (uint64_t)co->func;
    *((uint64_t *)stack_top + 1) = (uint64_t)co->arg;
    co->context.rsp = stack_top;

    /* 清零其他寄存器 */
    co->context.rbx = 0;
    co->context.r12 = 0;
    co->context.r13 = 0;
    co->context.r14 = 0;
    co->context.r15 = 0;

    /* ========== fswapcontext 快速上下文初始化 ========== */
    /* 快速上下文不包含 FPU 状态，使用 lazy FPU 保存 */
    co->fast_ctx.rsp = co->context.rsp;
    co->fast_ctx.rbp = co->context.rbp;
    co->fast_ctx.rbx = 0;
    co->fast_ctx.r12 = 0;
    co->fast_ctx.r13 = 0;
    co->fast_ctx.r14 = 0;
    co->fast_ctx.r15 = 0;
    co->fast_ctx.rip = (uint64_t)co_entry_wrapper;
    co->use_fast_switch = 1;  /* 默认启用快速切换 */
}

/* ========== 协程入口 (汇编调用) ========== */

void co_entry(co_func_t func, void *arg) {
    /* 获取当前协程 */
    struct coroutine *co = co_current();
    
    /* 执行协程函数 */
    if (func != NULL) {
        func(arg);
    }
    
    /* 标记完成 */
    if (co != NULL) {
        spin_lock(&co->lock);
        co->state = CO_STATE_FINISHED;
        co->end_time = 0;  /* TODO: 获取当前时间 */
        
        /* 如果有等待者，唤醒它 - 在释放锁前保存 waiter 本地副本 */
        struct coroutine *waiter = co->waiter;
        if (waiter != NULL) {
            co->waiter = NULL;  /* 清除等待者引用 */
        }
        spin_unlock(&co->lock);
        
        /* 在释放锁后唤醒等待者 */
        if (waiter != NULL) {
            spin_lock(&waiter->lock);
            waiter->state = CO_STATE_READY;
            waiter->wait_data = NULL;
            spin_unlock(&waiter->lock);
        }
    } else {
        /* co 为 NULL 时不需要解锁 */
    }
    
    /* 让出执行 (不再返回) */
    co_yield();
}

/* ========== 初始化 ========== */

int co_init(void) {
    INIT_LIST_HEAD(&all_coroutines);
    spinlock_init(&global_lock);
    
    /* 初始化所有调度器 */
    for (int i = 0; i < MAX_CO_SCHEDULERS; i++) {
        memset(&co_schedulers[i], 0, sizeof(struct co_scheduler));
        
        INIT_LIST_HEAD(&co_schedulers[i].pool.free_list);
        INIT_LIST_HEAD(&co_schedulers[i].pool.all_list);
        
        for (int j = 0; j < CO_PRIORITY_LEVELS; j++) {
            INIT_LIST_HEAD(&co_schedulers[i].pool.ready_queues[j]);
            co_schedulers[i].pool.ready_count[j] = 0;
        }
        
        spinlock_init(&co_schedulers[i].pool.lock);
        spinlock_init(&co_schedulers[i].lock);
        
        co_schedulers[i].policy = CO_SCHED_PRIORITY;
        co_schedulers[i].quantum = 5;
        co_schedulers[i].steal_enabled = 1;
    }
    
    return 0;
}

void co_cleanup(void) {
    /* 释放所有协程 */
    spin_lock(&global_lock);
    struct coroutine *co, *tmp;
    list_for_each_entry_safe(co, tmp, &all_coroutines, all_list) {
        list_del(&co->all_list);
        if (co->stack_base != NULL) {
            kfree(co->stack_base);
        }
        kfree(co);
    }
    spin_unlock(&global_lock);
}

/* ========== 协程创建/销毁 ========== */

struct coroutine *co_create(co_func_t func, void *arg,
                             size_t stack_size, uint8_t priority) {
    return co_create_named(NULL, func, arg, stack_size, priority);
}

struct coroutine *co_create_named(const char *name, co_func_t func,
                                   void *arg, size_t stack_size, uint8_t priority) {
    /* 调整栈大小 */
    if (stack_size == 0) {
        stack_size = CO_DEFAULT_STACK_SIZE;
    } else if (stack_size < CO_MIN_STACK_SIZE) {
        stack_size = CO_MIN_STACK_SIZE;
    } else if (stack_size > CO_MAX_STACK_SIZE) {
        stack_size = CO_MAX_STACK_SIZE;
    }
    
    /* 分配协程结构 */
    struct coroutine *co = kmalloc(sizeof(*co));
    if (co == NULL) {
        return NULL;
    }
    
    memset(co, 0, sizeof(*co));

    /* 分配栈 */
    co->stack_base = kmalloc(stack_size);
    if (co->stack_base == NULL) {
        kfree(co);
        return NULL;
    }
    
    co->stack_size = stack_size;
    
    /* 分配 ID */
    spin_lock(&global_lock);
    co->co_id = next_co_id++;
    spin_unlock(&global_lock);
    
    /* 设置属性 */
    if (name != NULL) {
        strncpy(co->name, name, CO_MAX_NAME_LEN - 1);
    }
    
    co->func = func;
    co->arg = arg;
    co->priority = priority > CO_PRIORITY_MAX ? CO_PRIORITY_MAX : priority;
    co->state = CO_STATE_CREATED;
    
    /* 获取当前线程 */
    struct thread *curr = thread_get_current();
    if (curr != NULL) {
        co->owner = curr;
        co->owner_tid = curr->tid;
    }
    
    spinlock_init(&co->lock);
    INIT_LIST_HEAD(&co->ready_list);
    INIT_LIST_HEAD(&co->wait_list);
    INIT_LIST_HEAD(&co->all_list);
    
    /* 初始化上下文 */
    co_context_init(co);
    
    /* 添加到全局链表 */
    spin_lock(&global_lock);
    list_add_tail(&co->all_list, &all_coroutines);
    spin_unlock(&global_lock);
    
    return co;
}

int co_destroy(struct coroutine *co) {
    if (co == NULL) {
        return -1;
    }
    
    spin_lock(&co->lock);
    
    /* 不能销毁运行中的协程 */
    if (co->state == CO_STATE_RUNNING) {
        spin_unlock(&co->lock);
        return -1;
    }
    
    spin_unlock(&co->lock);
    
    /* 从全局链表移除 */
    spin_lock(&global_lock);
    list_del(&co->all_list);
    spin_unlock(&global_lock);
    
    /* 释放资源 */
    if (co->stack_base != NULL) {
        kfree(co->stack_base);
    }
    kfree(co);
    
    return 0;
}

struct coroutine *co_current(void) {
    /* 获取当前线程的协程调度器 */
    struct co_scheduler *sched = co_get_scheduler();
    if (sched == NULL) {
        return NULL;
    }
    
    return sched->pool.current;
}

/* ========== 协程控制 ========== */

int co_start(struct coroutine *co) {
    if (co == NULL) {
        return -1;
    }
    
    spin_lock(&co->lock);
    
    if (co->state != CO_STATE_CREATED) {
        spin_unlock(&co->lock);
        return -1;
    }
    
    co->state = CO_STATE_READY;
    co->start_time = 0;  /* TODO: 获取当前时间 */
    
    spin_unlock(&co->lock);
    
    /* 添加到就绪队列 */
    struct co_scheduler *sched = co_get_scheduler();
    if (sched != NULL) {
        spin_lock(&sched->pool.lock);
        list_add_tail(&co->ready_list, 
                      &sched->pool.ready_queues[co->priority]);
        sched->pool.ready_count[co->priority]++;
        sched->pool.total_ready++;
        spin_unlock(&sched->pool.lock);
    }
    
    return 0;
}

void co_yield(void) {
    struct coroutine *current = co_current();
    if (current == NULL) {
        return;
    }
    
    /* 保存状态 */
    spin_lock(&current->lock);
    if (current->state == CO_STATE_RUNNING) {
        current->state = CO_STATE_READY;
    }
    current->switch_count++;
    spin_unlock(&current->lock);
    
    /* 添加回就绪队列 */
    struct co_scheduler *sched = co_get_scheduler();
    if (sched != NULL && current->state == CO_STATE_READY) {
        spin_lock(&sched->pool.lock);
        list_add_tail(&current->ready_list,
                      &sched->pool.ready_queues[current->priority]);
        sched->pool.ready_count[current->priority]++;
        sched->pool.total_ready++;
        spin_unlock(&sched->pool.lock);
    }
    
    /* 触发调度 */
    co_schedule();
}

void co_yield_timeout(uint64_t timeout_ns) {
    (void)timeout_ns;
    /* TODO: 实现带超时的让出 */
    co_yield();
}

int co_resume(struct coroutine *co) {
    if (co == NULL) {
        return -1;
    }
    
    spin_lock(&co->lock);
    
    if (co->state != CO_STATE_SUSPENDED && co->state != CO_STATE_WAITING) {
        spin_unlock(&co->lock);
        return -1;
    }
    
    co->state = CO_STATE_READY;
    spin_unlock(&co->lock);
    
    /* 添加到就绪队列 */
    struct co_scheduler *sched = co_get_scheduler();
    if (sched != NULL) {
        spin_lock(&sched->pool.lock);
        list_add_tail(&co->ready_list,
                      &sched->pool.ready_queues[co->priority]);
        sched->pool.ready_count[co->priority]++;
        sched->pool.total_ready++;
        spin_unlock(&sched->pool.lock);
    }
    
    return 0;
}

int co_suspend(struct coroutine *co) {
    if (co == NULL) {
        return -1;
    }
    
    spin_lock(&co->lock);
    co->state = CO_STATE_SUSPENDED;
    spin_unlock(&co->lock);
    
    return 0;
}

int co_cancel(struct coroutine *co) {
    if (co == NULL) {
        return -1;
    }
    
    spin_lock(&co->lock);
    
    if (co->state == CO_STATE_RUNNING) {
        spin_unlock(&co->lock);
        return -1;  /* 不能取消运行中的协程 */
    }
    
    co->state = CO_STATE_CANCELLED;
    spin_unlock(&co->lock);
    
    return 0;
}

int co_join(struct coroutine *co, int *result) {
    return co_join_timeout(co, result, 0);
}

int co_join_timeout(struct coroutine *co, int *result, uint64_t timeout_ns) {
    (void)timeout_ns;
    
    if (co == NULL) {
        return -1;
    }
    
    struct coroutine *current = co_current();
    
    spin_lock(&co->lock);
    
    if (co->state == CO_STATE_FINISHED || co->state == CO_STATE_CANCELLED) {
        /* 协程已完成 */
        if (result != NULL) {
            *result = co->result;
        }
        spin_unlock(&co->lock);
        return 0;
    }
    
    /* 设置等待者 */
    if (current != NULL) {
        co->waiter = current;
        current->state = CO_STATE_WAITING;
        current->wait_data = co;
    }
    
    spin_unlock(&co->lock);
    
    /* 让出执行 */
    co_yield();
    
    /* 恢复后检查结果 */
    spin_lock(&co->lock);
    if (result != NULL) {
        *result = co->result;
    }
    spin_unlock(&co->lock);
    
    return 0;
}

/* ========== 协程调度 ========== */

void co_schedule(void) {
    struct co_scheduler *sched = co_get_scheduler();
    if (sched == NULL) {
        return;
    }
    
    spin_lock(&sched->pool.lock);
    
    /* 查找就绪协程 */
    struct coroutine *next = NULL;
    for (int i = CO_PRIORITY_MAX; i >= CO_PRIORITY_MIN; i--) {
        if (!list_empty(&sched->pool.ready_queues[i])) {
            next = list_first_entry(&sched->pool.ready_queues[i],
                                    struct coroutine, ready_list);
            list_del(&next->ready_list);
            sched->pool.ready_count[i]--;
            sched->pool.total_ready--;
            break;
        }
    }
    
    if (next == NULL) {
        spin_unlock(&sched->pool.lock);

        /* 本地无就绪协程，尝试 work-stealing */
        if (sched->steal_enabled) {
            next = co_work_steal_auto();
            if (next != NULL) {
                spin_lock(&sched->pool.lock);
                next->state = CO_STATE_RUNNING;
                sched->pool.current = next;
                sched->switches++;
                spin_unlock(&sched->pool.lock);

                /* 切换到窃取的协程 */
                struct coroutine *prev = sched->pool.current;
                if (prev != NULL && prev->use_fast_switch && next->use_fast_switch) {
                    co_fswap(&prev->fast_ctx, &next->fast_ctx);
                } else if (prev != NULL) {
                    co_switch(&prev->context, &next->context);
                } else if (next->use_fast_switch) {
                    co_fswap(NULL, &next->fast_ctx);
                } else {
                    co_switch(NULL, &next->context);
                }
            }
        }
        return;  /* 没有就绪的协程 */
    }
    
    struct coroutine *prev = sched->pool.current;
    
    /* 更新状态 */
    next->state = CO_STATE_RUNNING;
    sched->pool.current = next;
    sched->switches++;
    
    spin_unlock(&sched->pool.lock);
    
    /* 切换上下文 - 使用 fswapcontext 优化路径 */
    if (prev != NULL && prev->state != CO_STATE_FINISHED) {
        if (prev->use_fast_switch && next->use_fast_switch) {
            /* 快速路径：仅保存 callee-saved 寄存器，跳过 FPU 保存 */
            co_fswap(&prev->fast_ctx, &next->fast_ctx);
        } else {
            /* 兼容路径：完整上下文切换 (含 FPU) */
            co_switch(&prev->context, &next->context);
        }
    } else {
        /* 首次切换或前一个协程已完成 */
        if (next->use_fast_switch) {
            co_fswap(NULL, &next->fast_ctx);
        } else {
            co_switch(NULL, &next->context);
        }
    }
}

void co_tick(void) {
    struct co_scheduler *sched = co_get_scheduler();
    if (sched == NULL) {
        return;
    }
    
    struct coroutine *current = sched->pool.current;
    if (current == NULL) {
        return;
    }
    
    /* 检查时间片 */
    current->run_time += 1000000;  /* 假设 1ms tick */
    
    /* 如果时间片用完，让出 */
    if (sched->policy == CO_SCHED_RR || sched->policy == CO_SCHED_PRIORITY) {
        if (current->run_time % (sched->quantum * 1000000) == 0) {
            co_yield();
        }
    }
}

struct coroutine *co_steal(struct co_scheduler *target) {
    if (target == NULL) {
        return NULL;
    }

    /* 工作窃取：从其他调度器偷取协程 */
    struct coroutine *stolen = NULL;

    spin_lock(&target->pool.lock);

    /* 从低优先级开始偷取，保留高优先级在原 CPU */
    for (int i = CO_PRIORITY_MIN; i <= CO_PRIORITY_MAX; i++) {
        if (!list_empty(&target->pool.ready_queues[i])) {
            /* 只偷取标记为可窃取的协程 */
            struct coroutine *co, *tmp;
            list_for_each_entry_safe(co, tmp, &target->pool.ready_queues[i], ready_list) {
                if (co->flags & CO_FLAG_STEALABLE) {
                    list_del(&co->ready_list);
                    target->pool.ready_count[i]--;
                    target->pool.total_ready--;
                    target->stolen_from++;
                    stolen = co;
                    goto steal_done;
                }
            }
            /* 没有可窃取的，取第一个 */
            stolen = list_first_entry(&target->pool.ready_queues[i],
                                      struct coroutine, ready_list);
            list_del(&stolen->ready_list);
            target->pool.ready_count[i]--;
            target->pool.total_ready--;
            target->stolen_from++;
            goto steal_done;
        }
    }

steal_done:
    spin_unlock(&target->pool.lock);

    return stolen;
}

/* ========== 增强工作窃取：空闲 CPU 自动窃取 ========== */

struct coroutine *co_work_steal_auto(void) {
    struct co_scheduler *sched = co_get_scheduler();
    if (sched == NULL || !sched->steal_enabled) {
        return NULL;
    }

    /* 检查本地队列是否为空 */
    if (sched->pool.total_ready > 0) {
        return NULL;  /* 本地有工作，不需要窃取 */
    }

    sched->steal_attempts++;

    struct coroutine *stolen = NULL;

    if (sched->steal_strategy == CO_STEAL_ROUND_ROBIN) {
        /* 轮询策略：依次尝试每个 CPU */
        for (uint32_t attempt = 0; attempt < MAX_CO_SCHEDULERS; attempt++) {
            uint32_t target_cpu = sched->steal_target % MAX_CO_SCHEDULERS;
            sched->steal_target++;

            if (target_cpu == sched->cpu_id) {
                continue;  /* 不偷自己 */
            }

            struct co_scheduler *target = &co_schedulers[target_cpu];
            if (target->pool.total_ready > 0) {
                stolen = co_steal(target);
                if (stolen != NULL) {
                    break;
                }
            }
        }
    } else {
        /* 随机策略：随机选择目标 CPU */
        uint32_t start = (uint32_t)(get_tick_count() & 0xFF) % MAX_CO_SCHEDULERS;
        for (uint32_t attempt = 0; attempt < MAX_CO_SCHEDULERS; attempt++) {
            uint32_t target_cpu = (start + attempt) % MAX_CO_SCHEDULERS;

            if (target_cpu == sched->cpu_id) {
                continue;
            }

            struct co_scheduler *target = &co_schedulers[target_cpu];
            if (target->pool.total_ready > 0) {
                stolen = co_steal(target);
                if (stolen != NULL) {
                    break;
                }
            }
        }
    }

    if (stolen != NULL) {
        sched->steal_successes++;
        /* 将窃取的协程加入本地就绪队列 */
        stolen->owner_tid = sched->cpu_id;
        spin_lock(&sched->pool.lock);
        list_add_tail(&stolen->ready_list,
                      &sched->pool.ready_queues[stolen->priority]);
        sched->pool.ready_count[stolen->priority]++;
        sched->pool.total_ready++;
        spin_unlock(&sched->pool.lock);
    } else {
        sched->steal_failures++;
    }

    return stolen;
}

/* 获取工作窃取统计 */
void co_steal_get_stats(uint64_t *attempts, uint64_t *successes,
                        uint64_t *failures, uint64_t *stolen_from) {
    uint64_t total_attempts = 0, total_successes = 0;
    uint64_t total_failures = 0, total_stolen_from = 0;

    for (int i = 0; i < MAX_CO_SCHEDULERS; i++) {
        total_attempts += co_schedulers[i].steal_attempts;
        total_successes += co_schedulers[i].steal_successes;
        total_failures += co_schedulers[i].steal_failures;
        total_stolen_from += co_schedulers[i].stolen_from;
    }

    if (attempts) *attempts = total_attempts;
    if (successes) *successes = total_successes;
    if (failures) *failures = total_failures;
    if (stolen_from) *stolen_from = total_stolen_from;
}

/* ========== 协程池预分配 ========== */

int co_pool_prealloc(struct co_pool *pool, uint32_t count) {
    if (pool == NULL || count == 0) {
        return -1;
    }

    /* 限制预分配数量不超过池最大值 */
    if (pool->count + count > pool->max_count) {
        count = pool->max_count - pool->count;
    }

    for (uint32_t i = 0; i < count; i++) {
        /* 分配协程结构体 */
        struct coroutine *co = kmalloc(sizeof(*co));
        if (co == NULL) {
            return (int)i;  /* 返回已预分配数量 */
        }

        /* 分配栈 */
        co->stack_base = kmalloc(CO_DEFAULT_STACK_SIZE);
        if (co->stack_base == NULL) {
            kfree(co);
            return (int)i;
        }

        memset(co, 0, sizeof(*co));
        co->stack_size = CO_DEFAULT_STACK_SIZE;
        co->state = CO_STATE_CREATED;
        co->priority = CO_PRIORITY_DEFAULT;
        co->flags = CO_FLAG_STEALABLE;  /* 预分配协程默认可窃取 */
        co->use_fast_switch = 1;

        spinlock_init(&co->lock);
        INIT_LIST_HEAD(&co->ready_list);
        INIT_LIST_HEAD(&co->wait_list);
        INIT_LIST_HEAD(&co->all_list);

        /* 分配 ID */
        spin_lock(&global_lock);
        co->co_id = next_co_id++;
        spin_unlock(&global_lock);

        /* 添加到池的空闲链表和全局链表 */
        spin_lock(&pool->lock);
        list_add_tail(&co->all_list, &pool->free_list);
        list_add_tail(&co->all_list, &pool->all_list);
        pool->count++;
        pool->total_created++;
        spin_unlock(&pool->lock);

        spin_lock(&global_lock);
        list_add_tail(&co->all_list, &all_coroutines);
        spin_unlock(&global_lock);
    }

    return 0;
}

/* 重置协程结构体以复用 */
void co_reset_for_reuse(struct coroutine *co, co_func_t func, void *arg) {
    if (co == NULL) {
        return;
    }

    /* 保留 co_id, stack_base, stack_size, lock, 链表节点 */
    co->func = func;
    co->arg = arg;
    co->state = CO_STATE_CREATED;
    co->priority = CO_PRIORITY_DEFAULT;
    co->switch_count = 0;
    co->run_time = 0;
    co->start_time = 0;
    co->end_time = 0;
    co->result = 0;
    co->waiter = NULL;
    co->wait_data = NULL;
    co->name[0] = '\0';
    co->use_fast_switch = 1;

    /* 重新初始化上下文 */
    co_context_init(co);
}

/* ========== 协程调度器管理 ========== */

struct co_scheduler *co_get_scheduler(void) {
    /* 获取当前 CPU 的调度器 */
    struct thread *curr = thread_get_current();
    if (curr == NULL) {
        return &co_schedulers[0];
    }
    
    uint32_t cpu = curr->cpu >= 0 ? (uint32_t)curr->cpu : 0;
    if (cpu >= MAX_CO_SCHEDULERS) {
        cpu = 0;
    }
    
    return &co_schedulers[cpu];
}

/* ========== 协程池 ========== */

struct co_pool *co_pool_create(uint32_t max_count) {
    struct co_pool *pool = kmalloc(sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    
    memset(pool, 0, sizeof(*pool));
    INIT_LIST_HEAD(&pool->free_list);
    INIT_LIST_HEAD(&pool->all_list);
    
    for (int i = 0; i < CO_PRIORITY_LEVELS; i++) {
        INIT_LIST_HEAD(&pool->ready_queues[i]);
    }
    
    pool->max_count = max_count > 0 ? max_count : CO_DEFAULT_POOL_SIZE;
    spinlock_init(&pool->lock);
    
    return pool;
}

void co_pool_destroy(struct co_pool *pool) {
    if (pool == NULL) {
        return;
    }
    
    /* 释放所有协程 */
    struct coroutine *co, *tmp;
    list_for_each_entry_safe(co, tmp, &pool->all_list, all_list) {
        list_del(&co->all_list);
        if (co->stack_base != NULL) {
            kfree(co->stack_base);
        }
        kfree(co);
    }
    
    kfree(pool);
}

struct coroutine *co_pool_alloc(struct co_pool *pool,
                                 co_func_t func, void *arg) {
    if (pool == NULL) {
        return NULL;
    }
    
    /* 先从空闲链表获取 */
    spin_lock(&pool->lock);
    
    if (!list_empty(&pool->free_list)) {
        struct coroutine *co = list_first_entry(&pool->free_list,
                                                 struct coroutine, all_list);
        list_del(&co->all_list);
        pool->count--;
        spin_unlock(&pool->lock);
        
        /* 重置协程 */
        co->func = func;
        co->arg = arg;
        co->state = CO_STATE_CREATED;
        co->switch_count = 0;
        co->run_time = 0;
        co_context_init(co);
        
        return co;
    }
    
    spin_unlock(&pool->lock);
    
    /* 创建新协程 */
    if (pool->count >= pool->max_count) {
        return NULL;  /* 池已满 */
    }
    
    struct coroutine *co = co_create(func, arg, 0, CO_PRIORITY_DEFAULT);
    if (co != NULL) {
        spin_lock(&pool->lock);
        list_add_tail(&co->all_list, &pool->all_list);
        pool->count++;
        pool->total_created++;
        spin_unlock(&pool->lock);
    }
    
    return co;
}

void co_pool_free(struct co_pool *pool, struct coroutine *co) {
    if (pool == NULL || co == NULL) {
        return;
    }
    
    spin_lock(&pool->lock);
    
    /* 添加到空闲链表 */
    list_add_tail(&co->all_list, &pool->free_list);
    pool->total_finished++;
    
    spin_unlock(&pool->lock);
}

/* ========== 协程通道 ========== */

struct co_channel *co_channel_create(const char *name, uint32_t capacity) {
    struct co_channel *chan = kmalloc(sizeof(*chan));
    if (chan == NULL) {
        return NULL;
    }
    
    memset(chan, 0, sizeof(*chan));
    
    if (capacity > 0) {
        chan->buffer = kmalloc(sizeof(void *) * capacity);
        if (chan->buffer == NULL) {
            kfree(chan);
            return NULL;
        }
        chan->capacity = capacity;
    }
    
    if (name != NULL) {
        strncpy(chan->name, name, CO_MAX_NAME_LEN - 1);
    }
    
    INIT_LIST_HEAD(&chan->send_waiters);
    INIT_LIST_HEAD(&chan->recv_waiters);
    spinlock_init(&chan->lock);
    
    return chan;
}

void co_channel_destroy(struct co_channel *chan) {
    if (chan == NULL) {
        return;
    }
    
    if (chan->buffer != NULL) {
        kfree(chan->buffer);
    }
    kfree(chan);
}

int co_channel_send(struct co_channel *chan, void *data) {
    if (chan == NULL) {
        return -1;
    }
    
    spin_lock(&chan->lock);
    
    /* 如果缓冲区满，等待 */
    while (chan->count >= chan->capacity) {
        struct coroutine *current = co_current();
        if (current == NULL) {
            spin_unlock(&chan->lock);
            return -1;
        }
        
        current->state = CO_STATE_WAITING;
        list_add_tail(&current->wait_list, &chan->send_waiters);
        spin_unlock(&chan->lock);
        
        co_yield();
        
        spin_lock(&chan->lock);
    }
    
    /* 发送数据 */
    if (chan->capacity > 0) {
        chan->buffer[chan->tail] = data;
        chan->tail = (chan->tail + 1) % chan->capacity;
    } else {
        /* 无缓冲通道：直接传递 */
        if (!list_empty(&chan->recv_waiters)) {
            struct coroutine *receiver = list_first_entry(&chan->recv_waiters,
                                                          struct coroutine, wait_list);
            list_del(&receiver->wait_list);
            receiver->wait_data = data;
            receiver->state = CO_STATE_READY;
        }
    }
    chan->count++;
    chan->sends++;
    
    /* 唤醒等待的接收者 */
    if (!list_empty(&chan->recv_waiters)) {
        struct coroutine *receiver = list_first_entry(&chan->recv_waiters,
                                                      struct coroutine, wait_list);
        list_del(&receiver->wait_list);
        receiver->state = CO_STATE_READY;
    }
    
    spin_unlock(&chan->lock);
    return 0;
}

int co_channel_recv(struct co_channel *chan, void **data) {
    if (chan == NULL || data == NULL) {
        return -1;
    }
    
    spin_lock(&chan->lock);
    
    /* 如果缓冲区空，等待 */
    while (chan->count == 0) {
        struct coroutine *current = co_current();
        if (current == NULL) {
            spin_unlock(&chan->lock);
            return -1;
        }
        
        current->state = CO_STATE_WAITING;
        list_add_tail(&current->wait_list, &chan->recv_waiters);
        spin_unlock(&chan->lock);
        
        co_yield();
        
        spin_lock(&chan->lock);
    }
    
    /* 接收数据 */
    *data = chan->buffer[chan->head];
    chan->head = (chan->head + 1) % chan->capacity;
    chan->count--;
    chan->recvs++;
    
    /* 唤醒等待的发送者 */
    if (!list_empty(&chan->send_waiters)) {
        struct coroutine *sender = list_first_entry(&chan->send_waiters,
                                                    struct coroutine, wait_list);
        list_del(&sender->wait_list);
        sender->state = CO_STATE_READY;
    }
    
    spin_unlock(&chan->lock);
    return 0;
}

int co_channel_try_send(struct co_channel *chan, void *data) {
    if (chan == NULL) {
        return -1;
    }
    
    spin_lock(&chan->lock);
    
    if (chan->count >= chan->capacity) {
        spin_unlock(&chan->lock);
        return -1;  /* 缓冲区满 */
    }
    
    chan->buffer[chan->tail] = data;
    chan->tail = (chan->tail + 1) % chan->capacity;
    chan->count++;
    chan->sends++;
    
    spin_unlock(&chan->lock);
    return 0;
}

int co_channel_try_recv(struct co_channel *chan, void **data) {
    if (chan == NULL || data == NULL) {
        return -1;
    }
    
    spin_lock(&chan->lock);
    
    if (chan->count == 0) {
        spin_unlock(&chan->lock);
        return -1;  /* 缓冲区空 */
    }
    
    *data = chan->buffer[chan->head];
    chan->head = (chan->head + 1) % chan->capacity;
    chan->count--;
    chan->recvs++;
    
    spin_unlock(&chan->lock);
    return 0;
}

/* ========== 协程同步原语 ========== */

void co_mutex_init(struct co_mutex *mutex) {
    if (mutex == NULL) return;
    mutex->locked = 0;
    INIT_LIST_HEAD(&mutex->waiters);
    spinlock_init(&mutex->lock);
}

void co_mutex_lock(struct co_mutex *mutex) {
    if (mutex == NULL) return;
    
    spin_lock(&mutex->lock);
    
    while (mutex->locked) {
        struct coroutine *current = co_current();
        if (current == NULL) {
            spin_unlock(&mutex->lock);
            return;
        }
        
        current->state = CO_STATE_WAITING;
        list_add_tail(&current->wait_list, &mutex->waiters);
        spin_unlock(&mutex->lock);
        
        co_yield();
        
        spin_lock(&mutex->lock);
    }
    
    mutex->locked = 1;
    spin_unlock(&mutex->lock);
}

int co_mutex_trylock(struct co_mutex *mutex) {
    if (mutex == NULL) return -1;
    
    spin_lock(&mutex->lock);
    
    if (mutex->locked) {
        spin_unlock(&mutex->lock);
        return -1;
    }
    
    mutex->locked = 1;
    spin_unlock(&mutex->lock);
    return 0;
}

void co_mutex_unlock(struct co_mutex *mutex) {
    if (mutex == NULL) return;
    
    spin_lock(&mutex->lock);
    
    mutex->locked = 0;
    
    /* 唤醒一个等待者 */
    if (!list_empty(&mutex->waiters)) {
        struct coroutine *waiter = list_first_entry(&mutex->waiters,
                                                    struct coroutine, wait_list);
        list_del(&waiter->wait_list);
        waiter->state = CO_STATE_READY;
    }
    
    spin_unlock(&mutex->lock);
}

void co_cond_init(struct co_cond *cond) {
    if (cond == NULL) return;
    INIT_LIST_HEAD(&cond->waiters);
    spinlock_init(&cond->lock);
}

void co_cond_wait(struct co_cond *cond, struct co_mutex *mutex) {
    if (cond == NULL) return;
    
    struct coroutine *current = co_current();
    if (current == NULL) return;
    
    spin_lock(&cond->lock);
    
    /* 释放互斥锁 */
    if (mutex != NULL) {
        co_mutex_unlock(mutex);
    }
    
    /* 加入等待队列 */
    current->state = CO_STATE_WAITING;
    list_add_tail(&current->wait_list, &cond->waiters);
    
    spin_unlock(&cond->lock);
    
    co_yield();
    
    /* 重新获取互斥锁 */
    if (mutex != NULL) {
        co_mutex_lock(mutex);
    }
}

void co_cond_signal(struct co_cond *cond) {
    if (cond == NULL) return;
    
    spin_lock(&cond->lock);
    
    if (!list_empty(&cond->waiters)) {
        struct coroutine *waiter = list_first_entry(&cond->waiters,
                                                    struct coroutine, wait_list);
        list_del(&waiter->wait_list);
        waiter->state = CO_STATE_READY;
    }
    
    spin_unlock(&cond->lock);
}

void co_cond_broadcast(struct co_cond *cond) {
    if (cond == NULL) return;
    
    spin_lock(&cond->lock);
    
    struct coroutine *waiter, *tmp;
    list_for_each_entry_safe(waiter, tmp, &cond->waiters, wait_list) {
        list_del(&waiter->wait_list);
        waiter->state = CO_STATE_READY;
    }
    
    spin_unlock(&cond->lock);
}

void co_sem_init(struct co_sem *sem, uint32_t count, uint32_t max) {
    if (sem == NULL) return;
    sem->count = count;
    sem->max = max;
    INIT_LIST_HEAD(&sem->waiters);
    spinlock_init(&sem->lock);
}

void co_sem_wait(struct co_sem *sem) {
    if (sem == NULL) return;
    
    spin_lock(&sem->lock);
    
    while (sem->count == 0) {
        struct coroutine *current = co_current();
        if (current == NULL) {
            spin_unlock(&sem->lock);
            return;
        }
        
        current->state = CO_STATE_WAITING;
        list_add_tail(&current->wait_list, &sem->waiters);
        spin_unlock(&sem->lock);
        
        co_yield();
        
        spin_lock(&sem->lock);
    }
    
    sem->count--;
    spin_unlock(&sem->lock);
}

void co_sem_post(struct co_sem *sem) {
    if (sem == NULL) return;
    
    spin_lock(&sem->lock);
    
    if (sem->max == 0 || (uint32_t)sem->count < sem->max) {
        sem->count++;
    }
    
    /* 唤醒一个等待者 */
    if (!list_empty(&sem->waiters)) {
        struct coroutine *waiter = list_first_entry(&sem->waiters,
                                                    struct coroutine, wait_list);
        list_del(&waiter->wait_list);
        waiter->state = CO_STATE_READY;
    }
    
    spin_unlock(&sem->lock);
}

/* ========== 统计 ========== */

int co_get_info(struct coroutine *co, co_state_t *state,
                uint64_t *switches, uint64_t *run_time) {
    if (co == NULL) {
        return -1;
    }
    
    spin_lock(&co->lock);
    
    if (state != NULL) *state = co->state;
    if (switches != NULL) *switches = co->switch_count;
    if (run_time != NULL) *run_time = co->run_time;
    
    spin_unlock(&co->lock);
    return 0;
}

void co_get_stats(uint64_t *created, uint64_t *finished, uint64_t *switches) {
    uint64_t total_created = 0, total_finished = 0, total_switches = 0;
    
    for (int i = 0; i < MAX_CO_SCHEDULERS; i++) {
        total_created += co_schedulers[i].pool.total_created;
        total_finished += co_schedulers[i].pool.total_finished;
        total_switches += co_schedulers[i].switches;
    }
    
    if (created != NULL) *created = total_created;
    if (finished != NULL) *finished = total_finished;
    if (switches != NULL) *switches = total_switches;
}

void co_print_status(void) {
    println("=== Coroutine Status ===");
    
    uint64_t created, finished, switches;
    co_get_stats(&created, &finished, &switches);
    
    print("  Created: ");
    print_dec(created);
    println("");
    
    print("  Finished: ");
    print_dec(finished);
    println("");
    
    print("  Switches: ");
    print_dec(switches);
    println("");
}
