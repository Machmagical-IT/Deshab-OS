#ifndef UTSM_PERCPU_H
#define UTSM_PERCPU_H

#include <utsm/types.h>

/*
 * R5：per-CPU 状态块。
 *
 * 寻址方式：wrmsr MSR_GS_BASE(0xC0000101) 指向本结构，%gs:0 读 self（CPU 索引）。
 * SAS-R0 全程 Ring0、单地址空间、无用户栈切换，因此不需要 swapgs，也不需要
 * TSS/IST：GS 基址在整机生命周期内不随任务切换改变。
 *
 * 约束：self 必须是结构首字段（%gs:0）。
 */

#define UTSM_MAX_CPUS 8

typedef struct utsm_percpu {
    u32 self;               /* @0：CPU 索引（= apic 列表序号，BSP=0） */
    u32 apic_id;            /* Local APIC ID */
    volatile u32 online;    /* 1 = 已进入 64 位 AP 入口并完成初始化 */
    volatile u32 parked;    /* 1 = 被 BSP 停驻（guest 执行/停机前） */
    volatile u32 need_resched;
    volatile u32 halted;    /* 1 = 已进入 halt 循环（panic/关机） */
    volatile u32 timer_start;  /* 1 = IPI 到达后编程并启动本 CPU LAPIC timer */
    volatile u32 parked_ack;   /* 1 = park 完成（任务已退回队列，运行 idle） */
    u32 current_slot;       /* 本 CPU 当前任务 slot（镜像 rq->current_task_slot） */
    u32 idle_slot;          /* 本 CPU idle 任务 slot */
    void *rq;               /* utsm_sched_run_queue*（void* 避免头文件环依赖） */
    u64 stack_top;          /* AP 启动栈顶（AP 入口写入自身 rsp 后不再使用） */
    u64 heartbeat_ns;
    u64 tick_count;
    u64 reserved[2];
} __attribute__((aligned(64))) utsm_percpu;

extern utsm_percpu g_percpu[UTSM_MAX_CPUS];
extern u32 g_cpu_count;         /* 已上线 CPU 数（BSP=1 起） */
extern u32 g_bsp_apic_id;

/* 取本 CPU 索引（%gs:0）。要求已调用 utsm_percpu_set_self。 */
static inline u32 utsm_cpu_index(void) {
    u32 v;
    __asm__ volatile("movl %%gs:0, %0" : "=r"(v));
    return v;
}

static inline utsm_percpu *utsm_this_cpu(void) {
    return &g_percpu[utsm_cpu_index()];
}

/* 把 GS_BASE 指向 g_percpu[cpu]（BSP 早期与每个 AP 入口各调用一次）。 */
void utsm_percpu_set_self(u32 cpu);

#endif /* UTSM_PERCPU_H */
