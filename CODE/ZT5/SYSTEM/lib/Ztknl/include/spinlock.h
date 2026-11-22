/*
 * Zratro OS - Spinlock Compatibility Header
 *
 * 基础定义已在 ktypes.h 中，此处仅做兼容引用
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "ktypes.h"

/* 所有 spinlock 函数已在 ktypes.h 中定义:
 *   spin_lock_init, spin_lock, spin_unlock, spin_trylock
 *   spin_lock_irqsave, spin_unlock_irqrestore
 */

#endif /* SPINLOCK_H */
