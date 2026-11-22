/* ===========================================================================
 *  Nutro+OS - IDT, PIC and IRQ dispatch
 * ===========================================================================
 */
#include "kernel.h"

typedef struct PACKED {
    u16 base_lo;
    u16 sel;
    u8  zero;
    u8  flags;
    u16 base_hi;
} idt_entry_t;

typedef struct PACKED {
    u16 limit;
    u32 base;
} idt_ptr_t;

typedef struct {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags;
} regs_t;

extern u32 isr_stub_table[];        /* 32 exception + 16 IRQ stubs */

static idt_entry_t idt[256];
static idt_ptr_t   idt_ptr;
static void (*irq_handlers[16])(void);

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

static void idt_set(int n, u32 handler)
{
    idt[n].base_lo = (u16)(handler & 0xFFFF);
    idt[n].sel     = 0x08;
    idt[n].zero    = 0;
    idt[n].flags   = 0x8E;          /* present, ring 0, 32-bit interrupt gate */
    idt[n].base_hi = (u16)(handler >> 16);
}

void pic_remap(void)
{
    u8 mask1 = inb(PIC1_DATA), mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();       /* master -> vectors 32..39 */
    outb(PIC2_DATA, 0x28); io_wait();       /* slave  -> vectors 40..47 */
    outb(PIC1_DATA, 0x04); io_wait();       /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void irq_install(int irq, void (*handler)(void))
{
    if (irq < 0 || irq > 15) return;
    irq_handlers[irq] = handler;
}

void irq_mask(int irq)
{
    u16 port;
    u8  bit;
    if (irq < 8) { port = PIC1_DATA; bit = (u8)irq; }
    else         { port = PIC2_DATA; bit = (u8)(irq - 8); }
    outb(port, (u8)(inb(port) | (1 << bit)));
}

void irq_unmask(int irq)
{
    u16 port;
    u8  bit;
    if (irq < 8) { port = PIC1_DATA; bit = (u8)irq; }
    else         { port = PIC2_DATA; bit = (u8)(irq - 8); }
    outb(port, (u8)(inb(port) & ~(1 << bit)));
    if (irq >= 8) outb(PIC1_DATA, (u8)(inb(PIC1_DATA) & ~(1 << 2)));
}

u32 irq_disable_save(void)
{
    u32 flags;
    __asm__ __volatile__("pushfl; popl %0" : "=r"(flags));
    interrupts_disable();
    return flags;
}

void irq_restore(u32 flags)
{
    if (flags & 0x200) interrupts_enable();
}

static const char *exception_names[32] = {
    "divide by zero", "debug", "NMI", "breakpoint", "overflow", "bound range",
    "invalid opcode", "device not available", "double fault", "coprocessor",
    "invalid TSS", "segment not present", "stack fault", "general protection",
    "page fault", "reserved", "x87 FPU", "alignment check", "machine check",
    "SIMD FPU", "virtualisation", "control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "hypervisor", "VMM",
    "security", "reserved"
};

static void exception_panic(regs_t *r)
{
    char buf[128];
    debug_write("\n*** EXCEPTION ");
    itoa((s32)r->int_no, buf);
    debug_write(buf);
    debug_write(" (");
    debug_write(r->int_no < 32 ? exception_names[r->int_no] : "?");
    debug_write(") err=");
    utohex(r->err_code, buf);
    debug_write(buf);
    debug_write(" eip=");
    utohex(r->eip, buf);
    debug_write(buf);
    debug_write(" ***\n");

    /* show a red panic screen */
    fb_set_target(fb_mem);
    fb_fill_screen(0x2A0808);
    char line[128];
    draw_text_scaled(40, 60, "Nutro+OS kernel panic", 0xFF8080, 3);
    ksnprintf(line, sizeof(line), "unhandled exception %u (%s)", r->int_no,
              r->int_no < 32 ? exception_names[r->int_no] : "?");
    draw_text(42, 140, line, 0xFFD0D0);
    ksnprintf(line, sizeof(line), "error code %x   eip %x   cs %x",
              r->err_code, r->eip, r->cs);
    draw_text(42, 160, line, 0xFFD0D0);
    for (;;) cpu_halt();
}

void isr_handler(regs_t *r)
{
    if (r->int_no < 32) {
        exception_panic(r);
        return;
    }
    u32 irq = r->int_no - 32;
    if (irq < 16 && irq_handlers[irq]) irq_handlers[irq]();

    if (r->int_no >= 40) outb(PIC2_CMD, PIC_EOI);
    if (r->int_no >= 32) outb(PIC1_CMD, PIC_EOI);
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    for (int i = 0; i < 48; i++) idt_set(i, isr_stub_table[i]);
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (u32)(unsigned long)idt;
    __asm__ __volatile__("lidt %0" : : "m"(idt_ptr));
    pic_remap();
    outb(PIC1_DATA, 0xFF);          /* everything masked until a driver asks */
    outb(PIC2_DATA, 0xFF);
}