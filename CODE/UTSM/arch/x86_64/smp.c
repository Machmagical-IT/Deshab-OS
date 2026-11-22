/* smp.c — SMP：BSP + 至多 SMP_MAX_CPUS 个 AP
 *
 * 两条使用路径（互斥）：
 *   1) 调度器多核（R5）：smp_early_init → smp_bringup_all → 每个 AP 先进入
 *      per-CPU idle 任务，再由 PCQ 调度器分派工作。
 *   2) 后台 guest（既有）：smp_init + smp_boot_ap(cpu, entry, arg) 把指定
 *      AP 直接交给 guest_runner（该路径下 AP 不参与调度）。
 *
 * trampoline（arch/x86_64/ap_trampoline.S）复制到物理 0x8000，参数块在
 * 0x8F00：+0x08 CR3 / +0x10 stack_top / +0x18 entry / +0x20 arg(→RDI)。
 */

#include <utsm/smp.h>
#include <utsm/percpu.h>
#include <utsm/types.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include <utsm/idt.h>
#include <utsm/gdt.h>
#include <utsm/lapic_timer.h>
#include <utsm/arena.h>
#include <utsm/sched_ext.h>
#include "limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;
extern volatile struct limine_memmap_request g_memmap_request;
extern volatile struct limine_rsdp_request g_rsdp_request;
extern u64 serial_tsc_per_ms(void);

extern char ap_trampoline_start[];
extern char ap_trampoline_end[];

#define AP_TRAMPOLINE_PHYS 0x8000ULL
#define AP_PARAM_PHYS      0x8F00ULL
#define AP_ICR_VECTOR      0x08u
#define AP_STACK_PAGES     4

#define LAPIC_EOI          0x0B0u
#define LAPIC_ICR_LOW      0x300u
#define LAPIC_ICR_HIGH     0x310u
#define LAPIC_ID           0x020u

#define ICR_DELIVERY_INIT  0x00000500u
#define ICR_DELIVERY_SIPI  0x00000600u
#define ICR_LEVEL_ASSERT   0x00004000u
#define ICR_TRIGGER_LEVEL  0x00008000u
#define ICR_DELIVERY_PEND  0x00001000u

/* ---- per-CPU 全局（percpu.h 约定） ---- */
utsm_percpu g_percpu[UTSM_MAX_CPUS];
u32 g_cpu_count = 1;
u32 g_bsp_apic_id;

static u32 g_cpu_n;                          /* MADT 解析到的 CPU 数 */
static u32 g_cpu_apic[SMP_MAX_CPUS];
static volatile u32 *g_lapic;
static int g_tramp_copied;
static int g_aps_started;
static u8  g_ctl_vec;
static int g_gs_ready;

/* 后台 guest 路径（既有 API） */
static volatile int g_ap_online;
static void (*g_ap_entry)(void *);
static void *g_ap_arg;
static u8 g_ap_stack[16384] __attribute__((aligned(16)));

static inline u64 rdmsr_(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static inline void wrmsr_g(u32 msr, u64 v) {
    u32 lo = (u32)v, hi = (u32)(v >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

void utsm_percpu_set_self(u32 cpu) {
    wrmsr_g(0xC0000101u, (u64)&g_percpu[cpu]);
    g_gs_ready = 1;
}

static u64 hhdm_offset(void) {
    return g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
}

static void delay_ms(u32 ms) {
    u64 tpm = serial_tsc_per_ms();
    if (!tpm) {
        for (volatile u64 i = 0; i < (u64)ms * 200000ULL; i++) __asm__("pause");
        return;
    }
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    u64 start = ((u64)hi << 32) | lo;
    u64 ticks = tpm * ms;
    for (;;) {
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        u64 now = ((u64)hi << 32) | lo;
        if (now - start >= ticks) break;
    }
}

static int lapic_ensure_mapped(void) {
    if (g_lapic) return 0;
    u64 base = rdmsr_(0x1B);
    u64 phys = base & 0xFFFFF000ULL;
    if (!phys) phys = 0xFEE00000ULL;
    u64 hhdm = hhdm_offset();
    if (!hhdm) return -1;
    g_lapic = (volatile u32 *)(hhdm + phys);
    (void)g_lapic[LAPIC_ID / 4];
    return 0;
}

static u32 lapic_read(u32 reg) {
    return g_lapic ? g_lapic[reg / 4] : 0;
}

static void lapic_write(u32 reg, u32 v) {
    if (g_lapic) g_lapic[reg / 4] = v;
}

static int mem_range_usable(u64 start, u64 end) {
    if (!g_memmap_request.response) return 0;
    struct limine_memmap_response *r = g_memmap_request.response;
    for (u64 i = 0; i < r->entry_count; i++) {
        struct limine_memmap_entry *e = r->entries[i];
        if (!e || e->type != LIMINE_MEMMAP_USABLE) continue;
        u64 s = e->base, t = e->base + e->length;
        if (s <= start && t >= end) return 1;
    }
    return 0;
}

/* ---- per-CPU 早期初始化 ---- */

void smp_early_init(void) {
    for (u32 c = 0; c < UTSM_MAX_CPUS; c++) {
        u8 *p = (u8 *)&g_percpu[c];
        for (u32 i = 0; i < sizeof(utsm_percpu); i++) p[i] = 0;
        g_percpu[c].self = c;
        g_percpu[c].current_slot = UTSM_SCHED_INVALID_SLOT;
        g_percpu[c].idle_slot = UTSM_SCHED_INVALID_SLOT;
    }
    u32 a, b, c_, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c_), "=d"(d) : "a"(1), "c"(0));
    (void)a; (void)c_; (void)d;
    g_bsp_apic_id = (b >> 24) & 0xFFu;
    g_percpu[0].apic_id = g_bsp_apic_id;
    g_percpu[0].online = 1;
    g_cpu_count = 1;
    utsm_percpu_set_self(0);
    log_info("[SMP] per-cpu init (BSP)");
    log_hex64("[SMP]   bsp_apic_id=", g_bsp_apic_id);
}

/* ---- RSDP / XSDT / MADT 解析 ---- */

struct acpi_sdt_hdr {
    char sig[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} __attribute__((packed));

static const struct acpi_sdt_hdr *find_madt(const void *rsdp) {
    if (!rsdp) return 0;
    const u8 *p = (const u8 *)rsdp;
    if (p[0] != 'R' || p[1] != 'S' || p[2] != 'D' || p[3] != ' ' ||
        p[4] != 'P' || p[5] != 'T' || p[6] != 'R' || p[7] != ' ') return 0;

    u8 rev = p[15];
    u64 xsdt_phys = 0, rsdt_phys = 0;
    if (rev >= 2) {
        xsdt_phys = *(const u64 *)(p + 24);
    }
    rsdt_phys = *(const u32 *)(p + 16);

    u64 hhdm = hhdm_offset();
    if (!hhdm) return 0;

    if (xsdt_phys) {
        const struct acpi_sdt_hdr *xsdt =
            (const struct acpi_sdt_hdr *)(hhdm + xsdt_phys);
        if (xsdt->sig[0] == 'X' && xsdt->sig[1] == 'S' &&
            xsdt->sig[2] == 'D' && xsdt->sig[3] == 'T') {
            u32 n = (xsdt->length - sizeof(*xsdt)) / 8;
            const u64 *entries = (const u64 *)(xsdt + 1);
            for (u32 i = 0; i < n; i++) {
                if (!entries[i]) continue;
                const struct acpi_sdt_hdr *t =
                    (const struct acpi_sdt_hdr *)(hhdm + entries[i]);
                if (t->sig[0] == 'A' && t->sig[1] == 'P' &&
                    t->sig[2] == 'I' && t->sig[3] == 'C') return t;
            }
        }
    }
    if (rsdt_phys) {
        const struct acpi_sdt_hdr *rsdt =
            (const struct acpi_sdt_hdr *)(hhdm + rsdt_phys);
        if (rsdt->sig[0] == 'R' && rsdt->sig[1] == 'S' &&
            rsdt->sig[2] == 'D' && rsdt->sig[3] == 'T') {
            u32 n = (rsdt->length - sizeof(*rsdt)) / 4;
            const u32 *entries = (const u32 *)(rsdt + 1);
            for (u32 i = 0; i < n; i++) {
                if (!entries[i]) continue;
                const struct acpi_sdt_hdr *t =
                    (const struct acpi_sdt_hdr *)(hhdm + entries[i]);
                if (t->sig[0] == 'A' && t->sig[1] == 'P' &&
                    t->sig[2] == 'I' && t->sig[3] == 'C') return t;
            }
        }
    }
    return 0;
}

int smp_init(void) {
    g_cpu_n = 0;
    g_bsp_apic_id = 0;
    g_ap_online = 0;
    g_ap_entry = 0;
    g_ap_arg = 0;

    if (rdmsr_(0x1B) & (1ULL << 10)) {
        panic_full("SMP-E02 X2APIC NOT SUPPORTED",
                   "background guest SMP requires the MMIO xAPIC interface", 0);
        return -1;
    }
    if (lapic_ensure_mapped() != 0) {
        log_warn("[SMP] LAPIC unavailable");
        return -1;
    }
    g_bsp_apic_id = lapic_read(LAPIC_ID) >> 24;

    const void *rsdp = g_rsdp_request.response ?
                       g_rsdp_request.response->address : 0;
    const struct acpi_sdt_hdr *madt = find_madt(rsdp);
    if (!madt) {
        log_warn("[SMP] MADT not found");
        return -1;
    }

    /* acpi_madt: header + u32 lapic_addr + u32 flags，然后变长条目 */
    const u8 *base = (const u8 *)madt;
    u32 off = (u32)sizeof(struct acpi_sdt_hdr) + 8;
    while (off + 2 <= madt->length && g_cpu_n < SMP_MAX_CPUS) {
        u8 type = base[off];
        u8 len = base[off + 1];
        if (len < 2 || off + len > madt->length) break;
        if (type == 0 && len >= 8) {
            u8 apic_id = base[off + 3];
            u32 flags = *(const u32 *)(base + off + 4);
            if (flags & 1u) {
                g_cpu_apic[g_cpu_n++] = apic_id;
            }
        }
        off += len;
    }

    /* 保证 index 0 是 BSP，避免 MADT 顺序把 BSP 排到 AP 槽。 */
    for (u32 i = 0; i < g_cpu_n; i++) {
        if (g_cpu_apic[i] == g_bsp_apic_id) {
            if (i != 0) {
                u32 t = g_cpu_apic[0];
                g_cpu_apic[0] = g_cpu_apic[i];
                g_cpu_apic[i] = t;
            }
            break;
        }
    }

    log_hex64("[SMP] BSP apic=", g_bsp_apic_id);
    log_hex64("[SMP] cpus=", g_cpu_n);
    if (g_cpu_n < 2) {
        log_warn("[SMP] only one enabled CPU");
        return -1;
    }
    return 0;
}

u32 smp_cpu_count(void) { return g_cpu_n ? g_cpu_n : 1; }
u32 smp_bsp_apic_id(void) { return g_bsp_apic_id; }
u32 smp_apic_id(u32 cpu_index) {
    return cpu_index < g_cpu_n ? g_cpu_apic[cpu_index] : 0xFFFFFFFFu;
}

u32 smp_current_cpu(void) {
    if (g_gs_ready) return utsm_cpu_index();
    if (!g_lapic) return 0;
    u32 id = lapic_read(LAPIC_ID) >> 24;
    for (u32 i = 0; i < g_cpu_n; i++) {
        if (g_cpu_apic[i] == id) return i;
    }
    return 0;
}

int smp_ap_online(void) { return g_ap_online; }

static int icr_wait_idle(void) {
    for (u64 spins = 0; spins < 100000000ULL; spins++) {
        if (!(lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_PEND)) return 0;
        __asm__("pause");
    }
    return -1;
}

static void icr_send(u32 apic_id, u32 mode) {
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, mode | ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);
}

/* SIPI 必须边沿触发：level+assert 会让 ICR 保持 pending，AP 被反复重启 */
static void icr_send_edge(u32 apic_id, u32 mode) {
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, mode);
}

void smp_send_ipi(u32 apic_id, u8 vector) {
    if (!g_lapic) return;
    icr_wait_idle();
    icr_send(apic_id, (u32)vector | 0x00000000u);
}

/* ---- 控制 IPI（park/resume/halt/timer start/resched） ---- */

static int smp_control_ipi(u8 vector) {
    (void)vector;
    lapic_timer_eoi();
    utsm_percpu *pc = utsm_this_cpu();
    if (pc->halted) {
        __asm__ volatile("cli" ::: "memory");
        for (;;) __asm__ volatile("hlt");
    }
    if (pc->parked) {
        lapic_timer_this_cpu_stop();
        utsm_sched_park_cpu();
        __atomic_store_n(&pc->parked_ack, 1u, __ATOMIC_RELEASE);
        return 1;
    }
    if (pc->timer_start) {
        pc->timer_start = 0;
        lapic_timer_this_cpu_init();
        lapic_timer_this_cpu_start();
    }
    if (pc->need_resched) {
        pc->need_resched = 0;
        utsm_sched_schedule();
    }
    return 1;
}

/* ---- AP C 入口（trampoline 以 RDI 传 arg） ---- */

void smp_ap_entry(u64 arg) {
    /* 后台 guest 路径：g_ap_entry 由 smp_boot_ap 设置 */
    if (g_ap_entry) {
        u32 gc = (u32)arg;
        utsm_percpu_set_self(gc);
        if (gc < UTSM_MAX_CPUS) {
            g_percpu[gc].apic_id = lapic_id_this_cpu();
            g_percpu[gc].online = 1;
        }
        g_ap_online = 1;
        idt_reload();
        log_info("[SMP] AP1 online");
        g_ap_entry(g_ap_arg);
        for (;;) { __asm__("cli; hlt"); }
    }

    /* 调度器路径：arg = cpu index */
    u32 cpu = (u32)arg;
    if (cpu >= UTSM_MAX_CPUS) cpu = 0;
    utsm_percpu_set_self(cpu);
    gdt_load_this_cpu();
    idt_load_this_cpu();

    u64 rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    utsm_sched_ap_bind_idle(cpu, rsp);

    utsm_percpu *pc = &g_percpu[cpu];
    pc->apic_id = lapic_id_this_cpu();
    pc->online = 1;

    log_info("[SMP] cpu online");
    log_hex64("[SMP]   cpu=", cpu);
    log_hex64("[SMP]   apic_id=", pc->apic_id);

    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

/* 复制 trampoline 并发送 INIT-SIPI-SIPI；stack_top 必须是 16 对齐-8。 */
static int boot_one_ap(u32 cpu, void (*entry)(void *), void *arg,
                       u64 stack_top, u32 timeout_ms) {
    u64 hhdm = hhdm_offset();
    if (!hhdm || !g_lapic) {
        panic_full("SMP-E05 SMP ENVIRONMENT MISSING",
                   "boot_one_ap requires HHDM and a mapped LAPIC", 0);
        return -1;
    }
    if (cpu == 0 || cpu >= g_cpu_n) {
        panic_full("SMP-E04 INVALID AP INDEX", "boot_one_ap cpu index invalid", 0);
        return -1;
    }
    if (!mem_range_usable(AP_TRAMPOLINE_PHYS, AP_PARAM_PHYS + 0x100ULL)) {
        panic_full("SMP-E01 AP LOW PAGE UNAVAILABLE",
                   "physical page 0x8000 is not usable for the AP trampoline", 0);
        return -1;
    }

    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 >> 32) {
        panic_full("SMP-E01 AP CR3 ABOVE 4G",
                   "the AP trampoline can only load a 32-bit CR3", 0);
        return -1;
    }

    if (!g_tramp_copied) {
        u64 blob_len = (u64)(ap_trampoline_end - ap_trampoline_start);
        if (blob_len > (AP_PARAM_PHYS - AP_TRAMPOLINE_PHYS)) {
            panic_full("SMP-E05 AP TRAMPOLINE TOO LARGE",
                       "the AP trampoline blob overlaps the parameter block", 0);
            return -1;
        }
        u8 *dst = (u8 *)(hhdm + AP_TRAMPOLINE_PHYS);
        for (u64 i = 0; i < blob_len; i++) dst[i] = (u8)ap_trampoline_start[i];
        g_tramp_copied = 1;
    }

    u64 *param = (u64 *)(hhdm + AP_PARAM_PHYS);
    for (u32 i = 0; i < 8; i++) param[i] = 0;
    param[0] = 0x41505452414D5043ULL;                 /* magic */
    param[1] = cr3;
    param[2] = stack_top;
    param[3] = (u64)&smp_ap_entry;
    param[4] = (u64)arg;                              /* RSI */
    param[5] = (u64)cpu;                              /* RDI：cpu 索引 */

    g_ap_entry = entry;
    g_ap_arg = arg;
    g_ap_online = 0;

    u32 target = g_cpu_apic[cpu];
    log_hex64("[SMP] boot AP apic=", target);

    if (icr_wait_idle() != 0) {
        panic_full("SMP-E02 AP ICR BUSY",
                   "LAPIC ICR stayed busy before INIT", 0);
        return -1;
    }
    icr_send(target, ICR_DELIVERY_INIT);
    delay_ms(10);
    if (icr_wait_idle() != 0) {
        panic_full("SMP-E02 AP ICR BUSY",
                   "LAPIC ICR stayed busy before SIPI", 0);
        return -1;
    }
    icr_send_edge(target, ICR_DELIVERY_SIPI | AP_ICR_VECTOR);
    delay_ms(1);
    if (icr_wait_idle() != 0) {
        panic_full("SMP-E02 AP ICR BUSY",
                   "LAPIC ICR stayed busy before the second SIPI", 0);
        return -1;
    }
    icr_send_edge(target, ICR_DELIVERY_SIPI | AP_ICR_VECTOR);

    /* 等目标 CPU 进入 C 入口 */
    u32 elapsed = 0;
    for (;;) {
        int online = entry ? g_ap_online : (int)g_percpu[cpu].online;
        if (online) return 0;
        delay_ms(1);
        if (++elapsed > timeout_ms) break;
    }
    panic_full("SMP-E02 AP START TIMEOUT",
               "the AP did not reach the C entry after INIT-SIPI-SIPI", 0);
    return -1;
}
int smp_prepare_guest_ap(u32 cpu_index) {
    if (cpu_index == 0 || cpu_index >= g_cpu_n) {
        panic_full("SMP-E04 INVALID AP INDEX",
                   "smp_prepare_guest_ap requested an invalid cpu", 0);
        return -1;
    }
    /* 不抬高 g_cpu_count：BSP 调度器仍是单核，AP 只跑 guest。
     * smp_halt_others 用 g_cpu_n 迭代，不受影响。 */
    g_percpu[0].apic_id = g_cpu_apic[0];
    g_percpu[cpu_index].self = cpu_index;
    g_percpu[cpu_index].apic_id = g_cpu_apic[cpu_index];
    g_percpu[cpu_index].online = 0;
    g_percpu[cpu_index].halted = 0;
    if (!g_ctl_vec) {
        int vec = irq_vector_alloc();
        if (vec < 0) {
            panic_full("SMP-E05 IPI VECTOR ALLOC FAILED",
                       "smp_prepare_guest_ap: control vector allocation failed", 0);
            return -1;
        }
        g_ctl_vec = (u8)vec;
        irq_register(g_ctl_vec, smp_control_ipi);
    }
    return 0;
}

/* ---- 既有后台 guest API ---- */

int smp_boot_ap(u32 cpu_index, void (*entry)(void *), void *arg) {
    if (cpu_index == 0 || cpu_index >= g_cpu_n) {
        panic_full("SMP-E04 INVALID AP INDEX",
                   "smp_boot_ap requested a CPU index outside the MADT list", 0);
        return -1;
    }
    if (!entry) {
        panic_full("SMP-E04 INVALID AP ENTRY",
                   "smp_boot_ap received a null entry point", 0);
        return -1;
    }
    if (g_aps_started) {
        /* 调度器已接管 AP：后台 guest 必须走同步路径（guest_runner 已拦截） */
        panic_full("SMP-E11 AP OWNED BY SCHEDULER",
                   "smp_boot_ap called after scheduler SMP bringup", 0);
        return -1;
    }
    return boot_one_ap(cpu_index, entry, arg,
                       (u64)&g_ap_stack[sizeof(g_ap_stack) - 8], 4000);
}

/* ---- 调度器多核 bringup ---- */

int smp_bringup_all(u32 ap_timeout_ms) {
    if (g_aps_started) return (int)(smp_online_count() - 1);
    if (smp_init() != 0) {
        log_info("[SMP] single cpu (no MADT/second cpu)");
        return 0;
    }

    u64 apic_base = rdmsr_(0x1B);
    if (apic_base & (1ULL << 10)) {
        panic_full("SMP-E02 X2APIC NOT SUPPORTED",
                   "smp_bringup_all: x2APIC mode, MMIO LAPIC required", 0);
        return -1;
    }

    u32 n = g_cpu_n;
    if (n > UTSM_MAX_CPUS) n = UTSM_MAX_CPUS;
    g_cpu_count = n;
    g_percpu[0].apic_id = g_cpu_apic[0];

    /* 控制 IPI 向量 */
    int vec = irq_vector_alloc();
    if (vec < 0) {
        panic_full("SMP-E05 IPI VECTOR ALLOC FAILED",
                   "smp_bringup_all: control vector allocation failed", 0);
        return -1;
    }
    g_ctl_vec = (u8)vec;
    irq_register(g_ctl_vec, smp_control_ipi);

    for (u32 c = 1; c < n; c++) {
        utsm_sched_cpu_init(c, 0);
        g_percpu[c].self = c;
        g_percpu[c].apic_id = g_cpu_apic[c];
        g_percpu[c].online = 0;

        if (utsm_sched_create_idle_for_cpu(c) < 0) {
            panic_full("SMP-E08 CPU SLOT EXHAUSTED",
                       "smp_bringup_all: idle task creation failed", 0);
            return -1;
        }
        void *stk = kmem_alloc_page(AP_STACK_PAGES);
        if (!stk) {
            panic_full("SMP-E08 CPU SLOT EXHAUSTED",
                       "smp_bringup_all: AP stack allocation failed", 0);
            return -1;
        }
        u64 top = ((u64)stk + AP_STACK_PAGES * 4096ULL) & ~15ULL;
        g_percpu[c].stack_top = top;

        if (boot_one_ap(c, 0, 0, top - 8, ap_timeout_ms) != 0) return -1;
    }

    g_aps_started = 1;
    log_info("[SMP] aps started");
    log_hex64("[SMP]   cpus=", g_cpu_count);
    return (int)(g_cpu_count - 1);
}

u32 smp_online_count(void) {
    u32 n = 0;
    for (u32 c = 0; c < g_cpu_count && c < UTSM_MAX_CPUS; c++) {
        if (g_percpu[c].online) n++;
    }
    return n;
}

int smp_is_enabled(void) { return g_aps_started && g_cpu_count > 1; }

void smp_request_resched(u32 cpu) {
    if (cpu >= UTSM_MAX_CPUS) return;
    utsm_percpu *pc = &g_percpu[cpu];
    pc->need_resched = 1;
    if (cpu == smp_current_cpu()) return;
    if (!pc->online || !g_ctl_vec) return;
    lapic_send_ipi(pc->apic_id, g_ctl_vec);
}

void smp_start_cpu_timers(void) {
    if (!g_aps_started || !g_ctl_vec) return;
    if (!lapic_timer_ready()) {
        panic_full("SMP-E10 LAPIC TIMER NOT READY",
                   "smp_start_cpu_timers: BSP calibration missing", 0);
        return;
    }
    for (u32 c = 1; c < g_cpu_count; c++) {
        if (!g_percpu[c].online) continue;
        g_percpu[c].timer_start = 1;
        lapic_send_ipi(g_percpu[c].apic_id, g_ctl_vec);
    }
    log_info("[SMP] per-cpu timers requested");
}

void smp_park_aps(void) {
    if (!g_aps_started || !g_ctl_vec) return;
    for (u32 c = 1; c < g_cpu_count; c++) {
        if (!g_percpu[c].online) continue;
        g_percpu[c].parked_ack = 0;
        g_percpu[c].parked = 1;
        lapic_send_ipi(g_percpu[c].apic_id, g_ctl_vec);
    }
    for (u32 t = 0; t < 100; t++) {
        int all = 1;
        for (u32 c = 1; c < g_cpu_count; c++) {
            if (g_percpu[c].online && !g_percpu[c].parked_ack) { all = 0; break; }
        }
        if (all) break;
        delay_ms(1);
    }
    log_info("[SMP] aps parked");
}

void smp_resume_aps(void) {
    if (!g_aps_started || !g_ctl_vec) return;
    for (u32 c = 1; c < g_cpu_count; c++) {
        if (!g_percpu[c].online) continue;
        g_percpu[c].parked = 0;
        g_percpu[c].parked_ack = 0;
        g_percpu[c].timer_start = 1;
        lapic_send_ipi(g_percpu[c].apic_id, g_ctl_vec);
    }
    log_info("[SMP] aps resumed");
}

void smp_halt_others(void) {
    u32 n = g_cpu_n ? g_cpu_n : g_cpu_count;
    if (!n) return;
    u32 self = smp_current_cpu();
    for (u32 c = 0; c < n; c++) {
        if (c == self) continue;
        if (!g_percpu[c].online) continue;
        g_percpu[c].halted = 1;
        if (g_ctl_vec && g_lapic) lapic_send_ipi(g_percpu[c].apic_id, g_ctl_vec);
    }
}
