/* ===========================================================================
 *  Nutro+OS - PS/2 keyboard driver (scancode set 1)
 * ===========================================================================
 */
#include "kernel.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBUF_SIZE  64

static key_event_t kbuf[KBUF_SIZE];
static volatile u32 khead, ktail;

static u8 shift_down, ctrl_down, alt_down, caps_on, extended;

/* unshifted / shifted ASCII for scancode set 1, index 0..0x39 */
static const char map_lo[64] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};
static const char map_hi[64] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static void kpush(u8 ch, u8 pressed)
{
    u32 next = (khead + 1) % KBUF_SIZE;
    if (next == ktail) return;              /* buffer full: drop */
    kbuf[khead].ch = ch;
    kbuf[khead].pressed = pressed;
    kbuf[khead].shift = shift_down;
    khead = next;
}

static void keyboard_irq(void)
{
    u8 sc = inb(KBD_DATA);

    if (sc == 0xE0) { extended = 1; return; }

    if (extended) {
        extended = 0;
        switch (sc & 0x7F) {
        case 0x48: kpush(KEY_UP, !(sc & 0x80));    return;
        case 0x50: kpush(KEY_DOWN, !(sc & 0x80));  return;
        case 0x4B: kpush(KEY_LEFT, !(sc & 0x80));  return;
        case 0x4D: kpush(KEY_RIGHT, !(sc & 0x80)); return;
        case 0x1C: kpush('\n', !(sc & 0x80));      return;
        default: return;
        }
    }

    if (sc == 0x2A || sc == 0x36) { shift_down = !(sc & 0x80); return; }
    if (sc == 0x3A) { if (!(sc & 0x80)) caps_on = !caps_on; return; }
    if (sc == 0x1D) { ctrl_down = !(sc & 0x80); return; }
    if (sc == 0x38) { alt_down = !(sc & 0x80); return; }

    u8 code = sc & 0x7F;
    u8 pressed = !(sc & 0x80);
    if (code >= 64) return;

    char ch = 0;
    if (shift_down) ch = map_hi[code];
    else            ch = map_lo[code];
    if (caps_on && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    else if (caps_on && shift_down && ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);

    if (ch || pressed) kpush((u8)ch, pressed);
}

int keyboard_get_event(key_event_t *ev)
{
    if (ktail == khead) return 0;
    *ev = kbuf[ktail];
    ktail = (ktail + 1) % KBUF_SIZE;
    return 1;
}

void keyboard_init(void)
{
    while (inb(KBD_STATUS) & 1) inb(KBD_DATA);   /* drain */
    khead = ktail = 0;
    irq_install(1, keyboard_irq);
    irq_unmask(1);
}