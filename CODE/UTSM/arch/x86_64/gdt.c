/* gdt.c — 内核自持 GDT（R5 多核）
 *
 * 每个 CPU 一套 GDT + TSS：x86 在 LTR 时会把 TSS 描述符的 busy 位置位，
 * 共享同一份描述符时第二个 CPU 再 ltr 就是"TSS busy" → #GP(0x28)
 * （启动时表现为 UTSM-EXC0D）。GDTR 本身也是 per-CPU 状态，所以这里
 * 按 CPU 索引各存一份，选择子编号保持一致（0x08/0x10/0x28）。
 *
 * 0x00 null
 * 0x08 代码：L=1, DPL=0, P=1, S=1, Type=0xA（exec/read）
 * 0x10 数据：DPL=0, P=1, S=1, Type=0x2（read/write）
 * 0x18/0x20 保留
 * 0x28 TSS（64 位描述符占 0x28..0x37 共 16 字节）
 */

#include <utsm/gdt.h>
#include <utsm/percpu.h>
#include <utsm/log.h>

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) gdt_ptr;

/* 64 位 TSS：SAS-R0 不切换栈，也没有 IST，但 VMX host-state 要求 host TR
 * 指向**当前 GDT 内**一个有效的 TSS 描述符。gdt_init() 用 lgdt 装上新 GDT
 * 后，固件遗留的 TR 选择子就指向越界描述符 → vmlaunch 报
 * "VM entry with invalid host-state fields"（error 0x8）。 */
typedef struct {
    u32 reserved0;
    u64 rsp[3];
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} __attribute__((packed)) tss64;

/* 每个 CPU 自己的 TSS 描述符必须处在自己的 GDT 内（LTR 只接受当前
 * GDTR 覆盖范围内的描述符）。 */
static tss64 g_tss[UTSM_MAX_CPUS];
static u64 g_gdt[UTSM_MAX_CPUS][7];
static u8 g_gdt_ready[UTSM_MAX_CPUS];   /* 1 = 本 CPU 已 ltr 过 */

#define TSS_SELECTOR 0x28u

static void gdt_reload(void) {
    /* 远返回重载 CS；随后重载数据段寄存器（故意跳过 GS）。 */
    __asm__ volatile(
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        :
        :
        : "rax", "memory");
}

static u32 gdt_cpu_index(void) {
    /* 早期（GS_BASE 未设置）%gs:0 读到的是无意义值；越界一律按 BSP 处理。 */
    u32 cpu = utsm_cpu_index();
    return cpu < UTSM_MAX_CPUS ? cpu : 0;
}

void gdt_load_this_cpu(void) {
    u32 cpu = gdt_cpu_index();
    u64 *gdt = g_gdt[cpu];
    int first = !g_gdt_ready[cpu];

    if (first) {
        tss64 *tss = &g_tss[cpu];
        for (u32 i = 0; i < sizeof(*tss); i++) ((u8 *)tss)[i] = 0;
        /* iomap_base 指向 TSS 末尾（= limit+1）→ 无 I/O 位图 */
        tss->iomap_base = (u16)sizeof(*tss);

        u64 base = (u64)tss;
        u64 limit = (u64)sizeof(*tss) - 1;

        gdt[0] = 0x0000000000000000ULL;                 /* null */
        gdt[1] = 0x00AF9A000000FFFFULL;                 /* 64 位代码 */
        gdt[2] = 0x00CF92000000FFFFULL;                 /* flat 数据 */
        gdt[3] = 0x0000000000000000ULL;
        gdt[4] = 0x0000000000000000ULL;
        gdt[5] = (limit & 0xFFFFULL)
               | ((base & 0xFFFFFFULL) << 16)
               | (0x89ULL << 40)                        /* S=0, Type=0x9 (64 位 TSS), P=1 */
               | (((limit >> 16) & 0xFULL) << 48)
               | (((base >> 24) & 0xFFULL) << 56);
        gdt[6] = (base >> 32) & 0xFFFFFFFFULL;

        g_gdt_ready[cpu] = 1;
        log_hex64("[GDT]   cpu=", cpu);
        log_hex64("[GDT]   tss_base=", base);
    }

    gdt_ptr p;
    p.limit = (u16)(sizeof(g_gdt[0]) - 1);
    p.base = (u64)&gdt[0];
    __asm__ volatile("lgdt %0" :: "m"(p) : "memory");
    gdt_reload();

    /* LTR 会把描述符 busy 位置 1，重复 ltr 同一选择子 = #GP(0x28)，
     * 因此每个 CPU 只装一次。 */
    if (first) {
        u16 tss_sel = (u16)TSS_SELECTOR;
        __asm__ volatile("ltr %0" :: "r"(tss_sel) : "memory");
    }
}

void gdt_init(void) {
    gdt_load_this_cpu();
    log_info("[GDT] kernel GDT installed");
    log_hex64("[GDT]   entries=", (u64)(sizeof(g_gdt[0]) / sizeof(g_gdt[0][0])));
}
