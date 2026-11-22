/*
 * Zratro OS - Lock Debugging Header
 * 
 * 内核锁调试和死锁检测
 */

#ifndef LOCKDEBUG_H
#define LOCKDEBUG_H

#include <stdint.h>
#include <stddef.h>

/* 锁类型 */
typedef enum {
    LOCK_TYPE_SPINLOCK,
    LOCK_TYPE_MUTEX,
    LOCK_TYPE_RWLOCK,
    LOCK_TYPE_SEMAPHORE
} lock_type_t;

/* ========== 初始化 ========== */

/* 初始化锁调试 */
int lockdebug_init(void);

/* ========== 锁注册 ========== */

/* 注册锁 */
int lockdebug_register(volatile void *lock, const char *name, lock_type_t type);

/* 注销锁 */
int lockdebug_unregister(volatile void *lock);

/* ========== 锁事件记录 ========== */

/* 记录锁获取 */
void lockdebug_acquire(volatile void *lock, const char *name, int type);

/* 记录锁释放 */
void lockdebug_release(volatile void *lock);

/* 记录锁等待 */
void lockdebug_wait(volatile void *lock, const char *name);

/* ========== 死锁检测 ========== */

/* 检查死锁 */
int lockdebug_check_deadlock(void);

/* 验证锁状态 */
int lockdebug_validate(volatile void *lock);

/* ========== 统计输出 ========== */

/* 打印锁统计 */
void lockdebug_stats(void);

/* 打印锁历史 */
void lockdebug_history(void);

/* ========== 控制接口 ========== */

/* 启用锁调试 */
void lockdebug_enable(void);

/* 禁用锁调试 */
void lockdebug_disable(void);

/* 检查是否启用 */
int lockdebug_is_enabled(void);

/* ========== 便捷宏 ========== */

/* 带调试的自旋锁获取 */
#define spin_lock_debug(lock, name) \
    do { \
        lockdebug_wait(lock, name); \
        while (__sync_lock_test_and_set(lock, 1)) { \
            asm volatile("pause"); \
        } \
        lockdebug_acquire(lock, name, LOCK_TYPE_SPINLOCK); \
    } while (0)

/* 带调试的自旋锁释放 */
#define spin_unlock_debug(lock) \
    do { \
        __sync_lock_release(lock); \
        lockdebug_release(lock); \
    } while (0)

#endif /* LOCKDEBUG_H */
