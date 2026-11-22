/* ===========================================================================
 *  Nutro+OS - PS/2 mouse driver (IRQ12, 3 byte packets)
 * ===========================================================================
 */
#include "kernel.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define MEV_SIZE 64

static volatile int g_x = 512, g_y = 384;
static volatile u8  g_buttons;

static mouse_event_t mev[MEV_SIZE];
static volatile u32 mhead, mtail;

static u8 packet[3];
static int packet_i;
static int inited;

static void mpush(s16 dx, s16 dy, u8 buttons, int is_button)
{
    u32 next = (mhead + 1) % MEV_SIZE;
    if (next == mtail) return;
    mev[mhead].dx = dx;
    mev[mhead].dy = dy;
    mev[mhead].buttons = buttons;
    mev[mhead].is_button = (u8)is_button;
    mhead = next;
}

static void ps2_wait_write(void)
{
    int t = 100000;
    while (t-- > 0 && (inb(PS2_STATUS) & 2)) ;
}

static void ps2_wait_read(void)
{
    int t = 100000;
    while (t-- > 0 && !(inb(PS2_STATUS) & 1)) ;
}

static void mouse_write(u8 cmd)
{
    ps2_wait_write();
    outb(PS2_CMD, 0xD4);       /* next byte goes to the aux device */
    ps2_wait_write();
    outb(PS2_DATA, cmd);
    ps2_wait_read();
    inb(PS2_DATA);
}

static u8 mouse_read(void)
{
    ps2_wait_read();
    return inb(PS2_DATA);
}

static void mouse_irq(void)
{
    u8 status = inb(PS2_STATUS);
    if (!(status & 0x20)) return;      /* not from the aux port */
    u8 b = inb(PS2_DATA);

    if (packet_i == 0 && !(b & 0x08)) return;   /* resynchronise */

    packet[packet_i++] = b;
    if (packet_i < 3) return;
    packet_i = 0;

    u8 f = packet[0];
    if (f & 0xC0) return;              /* overflow */

    s16 dx = (s16)packet[1];
    s16 dy = (s16)packet[2];
    if (f & 0x10) dx |= (s16)0xFF00;
    if (f & 0x20) dy |= (s16)0xFF00;
    dy = (s16)-dy;                     /* screen y grows downwards */

    /* byte 0 bits: 0 = left, 1 = right, 2 = middle on real hardware, but
       QEMU's PS/2 model reports the right button on bit 2 - fold it down so
       both conventions end up as "left = 1, right = 2" */
    u8 buttons = (u8)(f & 0x07);
    if (buttons & 0x04) buttons = (u8)((buttons & ~0x04) | 0x02);
    if (buttons != g_buttons) {
        g_buttons = buttons;
        mpush(dx, dy, buttons, 1);
    } else {
        mpush(dx, dy, buttons, 0);
    }

    g_x += dx;
    g_y += dy;
    if (g_x < 0) g_x = 0;
    if (g_y < 0) g_y = 0;
    if (g_x > (int)fb_width - 1) g_x = (int)fb_width - 1;
    if (g_y > (int)fb_height - 1) g_y = (int)fb_height - 1;
}

void mouse_init(void)
{
    ps2_wait_write();
    outb(PS2_CMD, 0xA8);               /* enable the aux port */
    ps2_wait_write();

    outb(PS2_CMD, 0x20);               /* read the config byte */
    u8 cfg = mouse_read();
    cfg |= 0x02;                       /* enable IRQ12 */
    cfg &= (u8)~0x20;                  /* enable the mouse clock */
    ps2_wait_write();
    outb(PS2_CMD, 0x60);
    ps2_wait_write();
    outb(PS2_DATA, cfg);
    ps2_wait_write();

    mouse_write(0xF6);                 /* set defaults */
    mouse_write(0xF4);                 /* enable reporting */

    g_x = (int)fb_width / 2;
    g_y = (int)fb_height / 2;
    g_buttons = 0;
    packet_i = 0;
    irq_install(12, mouse_irq);
    irq_unmask(12);
    inited = 1;
    debug_write("mouse: ps/2 initialised\n");
}

void mouse_get_pos(int *x, int *y)
{
    if (x) *x = g_x;
    if (y) *y = g_y;
}

int mouse_get_event(mouse_event_t *ev)
{
    if (mtail == mhead) return 0;
    *ev = mev[mtail];
    mtail = (mtail + 1) % MEV_SIZE;
    return 1;
}