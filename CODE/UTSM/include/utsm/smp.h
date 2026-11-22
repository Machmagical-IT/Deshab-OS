#ifndef UTSM_SMP_H
#define UTSM_SMP_H

#include <utsm/types.h>

/*
 * SMP：BSP + 至多 SMP_MAX_CPUS 个 AP。
 *
 * 两条使用路径：
 *   1) 调度器多核（R5）：smp_early_init → smp_bringup_all → 每个 AP 跑
 *      per-CPU idle 任务，由 PCQ 调度器分配工作；smp_start_cpu_timers /
 *      smp_park_aps / smp_resume_aps / smp_halt_others。
 *   2) 后台 guest（既有）：smp_init + smp_boot_ap(cpu, entry, arg) 把
 *      指定 AP 直接交给 guest_runner。两条路径互斥（调度器接管 AP 后
 *      guest 后台模式回落同步路径）。
 */

#define SMP_MAX_CPUS 8

/* ---- 调度器路径 ---- */

/* 最早调用（serial 之后）：per-CPU 表清零、BSP 条目、GS_BASE 装载。 */
void smp_early_init(void);

/* 解析 MADT 并把所有 AP 拉起为 scheduler idle。返回在线 AP 数（0=单核）。 */
int  smp_bringup_all(u32 ap_timeout_ms);

/* BSP tick 就绪后，让每个 AP 编程并启动自身 LAPIC timer。 */
void smp_start_cpu_timers(void);

u32  smp_online_count(void);
int  smp_is_enabled(void);         /* 1 = 调度器已接管至少一个 AP */

void smp_park_aps(void);           /* guest/停机前：AP 退回 idle 并停 timer */
void smp_resume_aps(void);
void smp_halt_others(void);        /* panic/关机：其它 CPU cli;hlt */

/* 请求指定 CPU 重调度（self 仅置标志，其它 CPU 发控制 IPI）。 */
void smp_request_resched(u32 cpu);

/* ---- 既有 API（后台 guest / 观测） ---- */

int  smp_init(void);               /* 解析 MADT + 映射 LAPIC；>=2 CPU 返回 0 */
int  smp_boot_ap(u32 cpu_index, void (*entry)(void *), void *arg);

/* 后台 guest 模式：在 BSP 上预备 AP 槽（g_cpu_count/per-cpu/控制 IPI），
 * 使 AP 上的 panic 能通过 smp_halt_others() 停下 BSP。 */
int  smp_prepare_guest_ap(u32 cpu_index);
u32  smp_cpu_count(void);
u32  smp_bsp_apic_id(void);
u32  smp_apic_id(u32 cpu_index);
u32  smp_current_cpu(void);
int  smp_ap_online(void);
void smp_send_ipi(u32 apic_id, u8 vector);

/* AP 从 trampoline 进入的 C 入口（RDI=参数，不返回）。 */
void smp_ap_entry(u64 arg);

#endif /* UTSM_SMP_H */
