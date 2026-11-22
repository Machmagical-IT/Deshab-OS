#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* GDT 段描述符数量 (TSS 占用 2 个槽位) */
#define GDT_ENTRIES 8

/* 段选择子 */
#define SEG_NULL    0x00
#define SEG_KCODE   0x08    /* 内核代码段 */
#define SEG_KDATA   0x10    /* 内核数据段 */
#define SEG_UCODE   0x18    /* 用户代码段 */
#define SEG_UDATA   0x20    /* 用户数据段 */
#define SEG_TSS     0x28    /* 任务状态段 */

/* 段描述符访问权限位 */
#define GDT_ACCESS_PRESENT      0x80
#define GDT_ACCESS_DPL0         0x00
#define GDT_ACCESS_DPL3         0x60
#define GDT_ACCESS_S_SYSTEM     0x00    /* 系统段 (TSS, LDT) */
#define GDT_ACCESS_S_CODE_DATA  0x10    /* 代码/数据段 */
#define GDT_ACCESS_TYPE_DATA    0x02    /* 数据段: 读/写 */
#define GDT_ACCESS_TYPE_CODE    0x0A    /* 代码段: 执行/读 */
#define GDT_ACCESS_TYPE_TSS     0x09    /* 64位 TSS (Available) */

/* GDT 标志位 */
#define GDT_FLAG_GRANULARITY_4K 0x80
#define GDT_FLAG_SIZE_32BIT     0x40
#define GDT_FLAG_LONG_MODE      0x20

/* GDT 描述符结构 (8字节) */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

/* GDT 指针结构 */
struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* 任务状态段 (TSS) - 64位模式 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];        /* RSP0, RSP1, RSP2 */
    uint64_t reserved1;
    uint64_t ist[7];        /* 中断栈表 (IST1-IST7) */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    /* I/O 位图基址 */
} __attribute__((packed));

/* 初始化 GDT 和 TSS */
void gdt_init(void);

/* 初始化 Per-CPU GDT */
void gdt_init_cpu(int cpu_id);

/* 设置 TSS 的内核栈 (RSP0) */
void tss_set_kernel_stack(uint64_t stack);

/* 加载 GDT (汇编实现) */
extern void gdt_load(struct gdt_ptr *gdt_ptr);

/* 加载 TSS (汇编实现) */
extern void tss_load(uint16_t selector);

/* 获取 GDT 指针 - 供 dual_kernel 使用 */
void *gdt_get_ptr(void);

/* 获取 Per-CPU TSS */
struct tss *gdt_get_per_cpu_tss(int cpu_id);

#endif
