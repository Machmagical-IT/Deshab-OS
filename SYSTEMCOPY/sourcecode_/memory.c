/* ===========================================================================
 *  Nutro+OS - physical memory bookkeeping and a simple bump heap
 * ===========================================================================
 */
#include "kernel.h"

/* provided by the linker script */
extern u8 __kernel_start[];
extern u8 __kernel_end[];

static u8 *heap_start = NULL;
static u8 *heap_end = NULL;
static u8 *heap_brk = NULL;

static const boot_info_t *g_bi = NULL;

u32 mem_total_kb(const boot_info_t *bi)
{
    u64 total = 0;
    const e820_entry_t *m = (const e820_entry_t *)(unsigned long)bi->e820_addr;
    for (u32 i = 0; i < bi->e820_count; i++)
        if (m[i].type == 1) total += m[i].length;
    if (!total) total = 64ull * 1024 * 1024;
    return (u32)(total / 1024);
}

u32 mem_usable_kb(const boot_info_t *bi)
{
    u64 total = 0;
    const e820_entry_t *m = (const e820_entry_t *)(unsigned long)bi->e820_addr;
    for (u32 i = 0; i < bi->e820_count; i++)
        if (m[i].type == 1) total += m[i].length;
    if (!total) total = 64ull * 1024 * 1024;
    return (u32)(total / 1024);
}

void heap_init(const boot_info_t *bi)
{
    g_bi = bi;
    u32 start = ((u32)(unsigned long)__kernel_end + 0xFFFF) & ~0xFFFFu;  /* 64 KiB margin */
    u32 end = start + 32u * 1024 * 1024;                                 /* default */

    const e820_entry_t *m = (const e820_entry_t *)(unsigned long)bi->e820_addr;
    for (u32 i = 0; i < bi->e820_count; i++) {
        if (m[i].type != 1) continue;
        u64 b = m[i].base, l = m[i].length;
        if (b <= start && b + l > start) {
            u64 e = b + l;
            if (e > 0xFFFFFFFFull) e = 0xFFFFFFFFull;
            u32 lim = (u32)e;
            /* stay clear of the top of RAM and keep the heap below 128 MiB */
            if (lim > start + 64u * 1024) lim -= 64u * 1024;
            u32 cap = start + 96u * 1024 * 1024;
            end = lim < cap ? lim : cap;
            break;
        }
    }
    if (end <= start + 1024u * 1024u) end = start + 4u * 1024 * 1024;
    heap_start = (u8 *)(unsigned long)start;
    heap_end = (u8 *)(unsigned long)end;
    heap_brk = heap_start;
}

void *kmalloc_aligned(u32 size, u32 align)
{
    if (!heap_start) return NULL;
    u32 p = (u32)(unsigned long)heap_brk;
    p = (p + align - 1) & ~(align - 1);
    if (p + size > (u32)(unsigned long)heap_end) return NULL;
    heap_brk = (u8 *)(unsigned long)(p + size);
    return (void *)(unsigned long)p;
}

void *kmalloc(u32 size)
{
    return kmalloc_aligned(size, 8);
}

u32 heap_used(void)
{
    if (!heap_start) return 0;
    return (u32)(heap_brk - heap_start);
}

u32 heap_size(void)
{
    if (!heap_start) return 0;
    return (u32)(heap_end - heap_start);
}