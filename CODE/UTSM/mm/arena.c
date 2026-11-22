#include <utsm/arena.h>
#include <utsm/config.h>
#include <utsm/log.h>
#include <utsm/spinlock.h>

static u8 arena[UTSM_ARENA_SIZE] __attribute__((aligned(UTSM_PAGE_SIZE)));
static u64 arena_offset;
static utsm_spinlock g_arena_lock;

void arena_init(void) {
    arena_offset = 0;
}

void *kmem_alloc_aligned(u64 size, u64 alignment) {
    /* R5：AP 与 BSP 可能并发申请（任务栈/idle 栈），bump 指针必须原子推进。
     * 持锁期间只做指针运算，不打印（OOM 日志在解锁后打）。 */
    u64 flags;
    utsm_spin_lock_irqsave(&g_arena_lock, &flags);
    u64 current = (u64)&arena[0] + arena_offset;
    u64 aligned = utsm_align_up_u64(current, alignment);
    u64 new_offset = (aligned - (u64)&arena[0]) + size;
    if (new_offset > UTSM_ARENA_SIZE) {
        utsm_spin_unlock_irqrestore(&g_arena_lock, flags);
        log_error("[UTSM] arena out of memory");
        return NULL;
    }
    arena_offset = new_offset;
    utsm_spin_unlock_irqrestore(&g_arena_lock, flags);
    return (void *)aligned;
}

void *kmem_alloc(u64 size) {
    return kmem_alloc_aligned(size, 16);
}

void *kmem_alloc_page(u64 page_count) {
    return kmem_alloc_aligned(page_count * UTSM_PAGE_SIZE, UTSM_PAGE_SIZE);
}

u64 kmem_arena_virtual_base(void) {
    return (u64)&arena[0];
}

u64 kmem_arena_virtual_end(void) {
    return (u64)&arena[0] + UTSM_ARENA_SIZE;
}
