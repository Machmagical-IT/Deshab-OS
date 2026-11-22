#ifndef UTSM_SPINLOCK_H
#define UTSM_SPINLOCK_H

#include <utsm/types.h>

/* 最小自旋锁：cli 关闭本 CPU 中断后 xchg 抢占。
 * 两套入口：
 *   - utsm_irq_save/restore + utsm_spin_lock_irqsave/unlock_irqrestore
 *     （arena/sched 等 R5 既有代码使用，调用方持有 flags）
 *   - utsm_spin_lock/unlock（新代码使用，锁结构自带 flags）
 * 禁止同一 CPU 递归获取同一把锁。 */
typedef struct {
    volatile u32 v;
    u64 flags;
} utsm_spinlock;

static inline u64 utsm_irq_save(void) {
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void utsm_irq_restore(u64 flags) {
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static inline void utsm_spin_lock_irqsave(utsm_spinlock *l, u64 *flags) {
    u64 f = utsm_irq_save();
    while (__atomic_exchange_n(&l->v, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
    *flags = f;
}

static inline void utsm_spin_unlock_irqrestore(utsm_spinlock *l, u64 flags) {
    __atomic_store_n(&l->v, 0u, __ATOMIC_RELEASE);
    utsm_irq_restore(flags);
}

static inline void utsm_spin_lock(utsm_spinlock *l) {
    u64 f;
    utsm_spin_lock_irqsave(l, &f);
    l->flags = f;
}

static inline void utsm_spin_unlock(utsm_spinlock *l) {
    utsm_spin_unlock_irqrestore(l, l->flags);
}

#endif
