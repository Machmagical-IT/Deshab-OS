/* ===========================================================================
 *  Nutro+OS - PIT timer
 * ===========================================================================
 */
#include "kernel.h"

#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define PIT_HZ   1193182u

static volatile u32 ticks;

static void timer_irq(void)
{
    ticks++;
}

void timer_init(u32 hz)
{
    if (hz == 0) hz = 100;
    u32 div = PIT_HZ / hz;
    outb(PIT_CMD, 0x36);                    /* channel 0, lobyte/hibyte, mode 3 */
    outb(PIT_CH0, (u8)(div & 0xFF));
    outb(PIT_CH0, (u8)((div >> 8) & 0xFF));
    irq_install(0, timer_irq);
    irq_unmask(0);
}

u32 timer_ticks(void) { return ticks; }

u32 timer_ms(void)
{
    /* 100 Hz base: ticks * 10 ms (works for any hz by scaling) */
    return ticks * 10u;
}

void timer_sleep_ms(u32 ms)
{
    u32 target = ticks + (ms + 9) / 10;
    while (ticks < target) cpu_halt();
}