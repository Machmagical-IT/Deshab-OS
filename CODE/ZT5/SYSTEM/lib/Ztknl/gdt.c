#include <stddef.h>
#include "gdt.h"
#include "heap.h"
#include "console.h"

/* GDT 表 */
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdt_ptr;

/* TSS */
static struct tss kernel_tss;

/* 设置 GDT 门描述符 */
static void gdt_set_gate(int num, uint32_t base, uint32_t limit, 
                         uint8_t access, uint8_t flags) {
    gdt[num].limit_low    = limit & 0xFFFF;
    gdt[num].base_low     = base & 0xFFFF;
    gdt[num].base_middle  = (base >> 16) & 0xFF;
    gdt[num].access       = access;
    gdt[num].granularity  = (limit >> 16) & 0x0F;
    gdt[num].granularity |= flags & 0xF0;
    gdt[num].base_high    = (base >> 24) & 0xFF;
}

/* 设置 TSS 描述符 (16字节，占用两个 GDT 表项) */
static void gdt_set_tss(int num, uint64_t base, uint64_t limit) {
    /* 低 8 字节 */
    uint32_t base_low  = base & 0xFFFFFFFF;
    uint32_t limit_low = limit & 0xFFFFF;
    
    gdt[num].limit_low    = limit_low & 0xFFFF;
    gdt[num].base_low     = base_low & 0xFFFF;
    gdt[num].base_middle  = (base_low >> 16) & 0xFF;
    gdt[num].access       = GDT_ACCESS_PRESENT | GDT_ACCESS_DPL0 | 
                            GDT_ACCESS_S_SYSTEM | GDT_ACCESS_TYPE_TSS;
    gdt[num].granularity  = ((limit_low >> 16) & 0x0F);
    gdt[num].base_high    = (base_low >> 24) & 0xFF;
    
    /* 高 8 字节 (存储 base 的高 32 位) */
    struct gdt_entry *tss_high = &gdt[num + 1];
    uint32_t *base_high_ptr = (uint32_t *)tss_high;
    base_high_ptr[0] = (base >> 32) & 0xFFFFFFFF;
    base_high_ptr[1] = 0;  /* 保留，必须为 0 */
}

void gdt_init(void) {
    /* 清空 TSS */
    for (size_t i = 0; i < sizeof(struct tss); i++) {
        ((uint8_t *)&kernel_tss)[i] = 0;
    }
    kernel_tss.iomap_base = sizeof(struct tss);  /* 无 I/O 位图 */
    
    /* Null 描述符 */
    gdt_set_gate(0, 0, 0, 0, 0);
    
    /* 内核代码段 (64位) */
    gdt_set_gate(1, 0, 0xFFFFF,
                 GDT_ACCESS_PRESENT | GDT_ACCESS_DPL0 | 
                 GDT_ACCESS_S_CODE_DATA | GDT_ACCESS_TYPE_CODE,
                 GDT_FLAG_LONG_MODE | GDT_FLAG_GRANULARITY_4K);
    
    /* 内核数据段 */
    gdt_set_gate(2, 0, 0xFFFFF,
                 GDT_ACCESS_PRESENT | GDT_ACCESS_DPL0 | 
                 GDT_ACCESS_S_CODE_DATA | GDT_ACCESS_TYPE_DATA,
                 GDT_FLAG_SIZE_32BIT | GDT_FLAG_GRANULARITY_4K);
    
    /* 用户代码段 (64位) */
    gdt_set_gate(3, 0, 0xFFFFF,
                 GDT_ACCESS_PRESENT | GDT_ACCESS_DPL3 | 
                 GDT_ACCESS_S_CODE_DATA | GDT_ACCESS_TYPE_CODE,
                 GDT_FLAG_LONG_MODE | GDT_FLAG_GRANULARITY_4K);
    
    /* 用户数据段 */
    gdt_set_gate(4, 0, 0xFFFFF,
                 GDT_ACCESS_PRESENT | GDT_ACCESS_DPL3 | 
                 GDT_ACCESS_S_CODE_DATA | GDT_ACCESS_TYPE_DATA,
                 GDT_FLAG_SIZE_32BIT | GDT_FLAG_GRANULARITY_4K);
    
    /* TSS 描述符 (占用索引 5 和 6) */
    gdt_set_tss(5, (uint64_t)&kernel_tss, sizeof(struct tss) - 1);
    
    /* 设置 GDT 指针 */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint64_t)&gdt;
    
    /* 加载 GDT */
    gdt_load(&gdt_ptr);
    
    /* 加载 TSS */
    tss_load(SEG_TSS);
}

/* 初始化 Per-CPU GDT (用于 AP 启动) */
void gdt_init_cpu(int cpu_id) {
    (void)cpu_id;
    
    /* 重新加载 GDT */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint64_t)&gdt;
    gdt_load(&gdt_ptr);
    
    /* 加载 TSS */
    tss_load(SEG_TSS);
    
    /* 重新加载段寄存器 (不包括 GS，因为它存储了 CPU 本地数据指针) */
    asm volatile("mov $0x10, %%ax; mov %%ax, %%ds; mov %%ax, %%es; "
                 "mov %%ax, %%fs; mov %%ax, %%ss" ::: "ax");
}

void tss_set_kernel_stack(uint64_t stack) {
    kernel_tss.rsp[0] = stack;
}

/* ========== GDT 高级增强功能 ========== */

/* 辅助函数: 在指定 GDT 中设置门 */
static void gdt_set_gate_cpu(struct gdt_entry *gdt_table, int num, 
                             uint32_t base, uint32_t limit, 
                             uint8_t access, uint8_t flags) {
    gdt_table[num].base_low    = base & 0xFFFF;
    gdt_table[num].base_middle = (base >> 16) & 0xFF;
    gdt_table[num].base_high   = (base >> 24) & 0xFF;
    gdt_table[num].limit_low   = limit & 0xFFFF;
    gdt_table[num].granularity = ((limit >> 16) & 0x0F) | (flags & 0xF0);
    gdt_table[num].access      = access;
}

/* 辅助函数: 在指定 GDT 中设置 TSS */
static void gdt_set_tss_cpu(struct gdt_entry *gdt_table, int num, 
                            uint64_t base, uint32_t limit) {
    uint32_t base_low = base & 0xFFFFFFFF;
    
    gdt_table[num].limit_low   = limit & 0xFFFF;
    gdt_table[num].base_low    = base_low & 0xFFFF;
    gdt_table[num].base_middle = (base_low >> 16) & 0xFF;
    gdt_table[num].access      = GDT_ACCESS_PRESENT | GDT_ACCESS_DPL0 | 
                                 GDT_ACCESS_S_SYSTEM | GDT_ACCESS_TYPE_TSS;
    gdt_table[num].granularity = ((limit >> 16) & 0x0F);
    gdt_table[num].base_high   = (base_low >> 24) & 0xFF;
    
    struct gdt_entry *tss_high = &gdt_table[num + 1];
    uint32_t *base_high_ptr = (uint32_t *)tss_high;
    base_high_ptr[0] = (base >> 32) & 0xFFFFFFFF;
    base_high_ptr[1] = 0;
}

/* === Per-CPU GDT 支持 === */

#define GDT_MAX_CPUS       256
#define MAX_PROCESSES      1024

/* Per-CPU TSS */
static struct tss per_cpu_tss[GDT_MAX_CPUS];

/* Per-CPU GDT */
static struct gdt_entry per_cpu_gdt[GDT_MAX_CPUS][GDT_ENTRIES];
static struct gdt_ptr per_cpu_gdt_ptr[GDT_MAX_CPUS];

/* 初始化 Per-CPU GDT */
int gdt_init_per_cpu(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= GDT_MAX_CPUS) return -1;
    
    struct tss *tss = &per_cpu_tss[cpu_id];
    struct gdt_entry *gdt_table = per_cpu_gdt[cpu_id];
    
    /* 清空 TSS */
    for (size_t i = 0; i < sizeof(struct tss); i++) {
        ((uint8_t *)tss)[i] = 0;
    }
    tss->iomap_base = sizeof(struct tss);
    
    /* Null 描述符 */
    gdt_set_gate_cpu(gdt_table, 0, 0, 0, 0, 0);
    
    /* 内核代码段 */
    gdt_set_gate_cpu(gdt_table, 1, 0, 0xFFFFF,
                     GDT_ACCESS_PRESENT | GDT_ACCESS_DPL0 | 
                     GDT_ACCESS_S_CODE_DATA | GDT_ACCESS_TYPE_CODE,
                     GDT_FLAG_LONG_MODE | GDT_FLAG_GRANULARITY_4K);
    
    /* 内核数据段 */
    gdt_set_gate_cpu(gdt_table, 2, 0, 0xFFFFF,
                     GDT_ACCESS_PRESENT | GDT_ACCESS_DPL0 | 
                     GDT_ACCESS_S_CODE_DATA | GDT_ACCESS_TYPE_DATA,
                     GDT_FLAG_SIZE_32BIT | GDT_FLAG_GRANULARITY_4K);
    
    /* 用户代码段 */
    gdt_set_gate_cpu(gdt_table, 3, 0, 0xFFFFF,
                     GDT_ACCESS_PRESENT | GDT_ACCESS_DPL3 | 
                     GDT_ACCESS_S_CODE_DATA | GDT_ACCESS_TYPE_CODE,
                     GDT_FLAG_LONG_MODE | GDT_FLAG_GRANULARITY_4K);
    
    /* 用户数据段 */
    gdt_set_gate_cpu(gdt_table, 4, 0, 0xFFFFF,
                     GDT_ACCESS_PRESENT | GDT_ACCESS_DPL3 | 
                     GDT_ACCESS_S_CODE_DATA | GDT_ACCESS_TYPE_DATA,
                     GDT_FLAG_SIZE_32BIT | GDT_FLAG_GRANULARITY_4K);
    
    /* TSS */
    gdt_set_tss_cpu(gdt_table, 5, (uint64_t)tss, sizeof(struct tss) - 1);
    
    /* 设置 GDT 指针 */
    per_cpu_gdt_ptr[cpu_id].limit = sizeof(gdt_table) - 1;
    per_cpu_gdt_ptr[cpu_id].base = (uint64_t)gdt_table;
    
    return 0;
}

/* 设置 Per-CPU 内核栈 */
void gdt_set_per_cpu_kernel_stack(int cpu_id, uint64_t stack) {
    if (cpu_id >= 0 && cpu_id < GDT_MAX_CPUS) {
        per_cpu_tss[cpu_id].rsp[0] = stack;
    }
}

/* 获取 Per-CPU TSS */
struct tss *gdt_get_per_cpu_tss(int cpu_id) {
    if (cpu_id >= 0 && cpu_id < GDT_MAX_CPUS) {
        return &per_cpu_tss[cpu_id];
    }
    return NULL;
}

/* === 描述符管理增强 === */

/* 获取描述符信息 */
int gdt_get_descriptor_info(int seg, uint64_t *base, uint32_t *limit, 
                            uint8_t *access, uint8_t *flags) {
    if (seg < 0 || seg >= GDT_ENTRIES) return -1;
    
    struct gdt_entry *e = &gdt[seg];
    
    if (base) {
        *base = ((uint64_t)e->base_high << 24) | 
                ((uint64_t)e->base_middle << 16) | 
                e->base_low;
    }
    
    if (limit) {
        uint32_t lim = ((uint32_t)(e->granularity & 0x0F) << 16) | e->limit_low;
        if (e->granularity & 0x80) {
            lim = (lim << 12) | 0xFFF;
        }
        *limit = lim;
    }
    
    if (access) *access = e->access;
    if (flags) *flags = e->granularity;
    
    return 0;
}

/* 检查描述符是否有效 */
int gdt_is_descriptor_valid(int seg) {
    if (seg < 0 || seg >= GDT_ENTRIES) return 0;
    return gdt[seg].access & GDT_ACCESS_PRESENT;
}

/* === 权限管理 === */

/* 获取描述符特权级 */
int gdt_get_descriptor_dpl(int seg) {
    if (seg < 0 || seg >= GDT_ENTRIES) return -1;
    return (gdt[seg].access >> 5) & 0x03;
}

/* 设置描述符特权级 */
void gdt_set_descriptor_dpl(int seg, int dpl) {
    if (seg < 0 || seg >= GDT_ENTRIES) return;
    gdt[seg].access = (gdt[seg].access & ~0x60) | ((dpl & 0x03) << 5);
}

/* === LDT 支持 === */

/* LDT 描述符 */
struct ldt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

#define MAX_LDT_ENTRIES    8192

static struct ldt_entry *process_ldt[MAX_PROCESSES];
static int ldt_count[MAX_PROCESSES];

/* 分配 LDT */
int gdt_alloc_ldt(int pid, int entries) {
    if (pid < 0 || pid >= MAX_PROCESSES) return -1;
    if (entries <= 0 || entries > MAX_LDT_ENTRIES) return -1;
    
    process_ldt[pid] = kmalloc(sizeof(struct ldt_entry) * entries);
    if (process_ldt[pid] == NULL) return -1;
    
    ldt_count[pid] = entries;
    return 0;
}

/* 释放 LDT */
void gdt_free_ldt(int pid) {
    if (pid >= 0 && pid < MAX_PROCESSES && process_ldt[pid] != NULL) {
        kfree(process_ldt[pid]);
        process_ldt[pid] = NULL;
        ldt_count[pid] = 0;
    }
}

/* === 调用门 === */

/* 调用门描述符 */
struct call_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  param_count : 5;
    uint8_t  reserved : 3;
    uint8_t  access;
    uint16_t offset_high;
} __attribute__((packed));

/* 设置调用门 */
void gdt_set_call_gate(int num, uint16_t selector, uint64_t offset, 
                       int dpl, int param_count) {
    if (num < 0 || num >= GDT_ENTRIES) return;
    
    struct call_gate *gate = (struct call_gate *)&gdt[num];
    gate->offset_low = offset & 0xFFFF;
    gate->offset_high = (offset >> 16) & 0xFFFF;
    gate->selector = selector;
    gate->param_count = param_count & 0x1F;
    gate->reserved = 0;
    gate->access = GDT_ACCESS_PRESENT | (dpl << 5) | 0x04;
}

/* === TSS 扩展功能 === */

/* 设置 TSS I/O 位图 */
void tss_set_iomap(uint64_t iomap_addr) {
    kernel_tss.iomap_base = iomap_addr;
}

/* 设置 TSS 中断重定向 */
void tss_set_interrupt_redirection(int int_num, int redirect_to) {
    uint8_t *redir = (uint8_t *)(&kernel_tss + 1);
    if (redirect_to) {
        redir[int_num / 8] |= (1 << (int_num % 8));
    } else {
        redir[int_num / 8] &= ~(1 << (int_num % 8));
    }
}

/* 设置 TSS 调试陷阱 */
void tss_set_debug_trap(int enable) {
    if (enable) {
        kernel_tss.rsp[0] |= 0x01;
    }
}

/* === GDT 统计 === */

struct gdt_stats {
    int total_entries;
    int valid_entries;
    int code_segments;
    int data_segments;
    int system_segments;
};

/* 获取 GDT 统计 */
void gdt_get_stats(struct gdt_stats *stats) {
    if (stats == NULL) return;
    
    stats->total_entries = GDT_ENTRIES;
    stats->valid_entries = 0;
    stats->code_segments = 0;
    stats->data_segments = 0;
    stats->system_segments = 0;
    
    for (int i = 0; i < GDT_ENTRIES; i++) {
        if (gdt[i].access & GDT_ACCESS_PRESENT) {
            stats->valid_entries++;
            
            uint8_t type = gdt[i].access & 0x0F;
            if (type >= 0x08 && type <= 0x0B) {
                stats->code_segments++;
            } else if (type >= 0x00 && type <= 0x03) {
                stats->data_segments++;
            } else {
                stats->system_segments++;
            }
        }
    }
}

/* === 诊断报告 === */

/* 获取 GDT 指针 - 供 dual_kernel 使用 */
void *gdt_get_ptr(void) {
    return (void *)&gdt_ptr;
}

void gdt_print_report(void) {
    println("╔══════════════════════════════════════════╗");
    println("║          GDT Diagnostic Report           ║");
    println("╚══════════════════════════════════════════╝");

    println("");
    println("=== GDT 状态 ===");
    print("GDT 基址: 0x"); print_hex(gdt_ptr.base); println("");
    print("GDT 限制: "); print_dec(gdt_ptr.limit); println(" bytes");

    println("");
    println("=== 描述符统计 ===");
    struct gdt_stats stats;
    gdt_get_stats(&stats);
    print("有效条目: "); print_dec(stats.valid_entries); println("");
    print("代码段: "); print_dec(stats.code_segments); println("");
    print("数据段: "); print_dec(stats.data_segments); println("");
    print("系统段: "); print_dec(stats.system_segments); println("");

    println("");
    println("=== TSS 状态 ===");
    print("TSS 地址: 0x"); print_hex((uint64_t)&kernel_tss); println("");
    print("IOMAP 基址: "); print_dec(kernel_tss.iomap_base); println("");
    print("内核栈 RSP0: 0x"); print_hex(kernel_tss.rsp[0]); println("");
}
