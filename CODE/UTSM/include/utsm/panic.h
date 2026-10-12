#ifndef UTSM_PANIC_H
#define UTSM_PANIC_H

#include <utsm/types.h>

/* ===================================================================
 *  F1: 内核 panic 统一兜底（莲花崩溃屏）
 *
 *  崩了也得崩出莲花：
 *    - 帧缓冲直接绘制（不走 console_fb 驱动——崩溃时驱动可能已挂）
 *    - 淡金莲花 + panic_symbol + 消息 + 寄存器简表
 *    - DRR 归档（crash_buf）后 halt
 *
 *  本路径只依赖：Limine framebuffer（Limine 常驻映射）+ serial + DRR
 *  Emergency Pool（静态区），不依赖堆/驱动/中断。
 * =================================================================== */

/* 寄存器简表（可传 0：无异常帧的普通 panic） */
typedef struct panic_regs {
    u64 rip;
    u64 rsp;
    u64 rflags;
    u64 cs;
    u64 err;
    u64 vector;
    u64 cr2;
    u64 rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11;
} panic_regs;

/* ===================================================================
 *  panic 模式（FUCK [panic] mode=strict|fallback，默认 strict）
 *
 *  strict  ：panic_full 画莲花屏 + DRR 归档 + halt，不返回（零降级铁律）。
 *  fallback：panic_full 只留串口坐标 + 计数后返回，调用点必须紧跟一条
 *            可执行的备用路径（返回错误码 / 跳过该能力 / 回退实现）。
 *            寄存器帧非空（CPU 异常）时无论模式都 halt。
 *
 *  panic_halt 无视模式恒 halt，仅用于"没有任何可继续的状态"的终点：
 *  CPU 异常、LA57 激活（4 级页表 walk 会写坏内存）、DSK 交接失败、
 *  panic_test 演示钩子。
 * =================================================================== */

/* 统一 panic 入口：strict 模式不返回；fallback 模式记录后返回 */
void panic_full(const char *symbol, const char *msg, const panic_regs *regs);

/* 恒 halt 的 panic（不受 fallback 模式影响） */
__attribute__((noreturn))
void panic_halt(const char *symbol, const char *msg, const panic_regs *regs);

/* 设置/查询 fallback 模式（kernel/main.c 读 FUCK 后设置一次） */
void panic_set_fallback(int enable);
int panic_fallback_enabled(void);
/* fallback 模式下被吞掉的 panic 次数 */
u32 panic_fallback_hits(void);

/* 兼容旧接口：panic("msg") = panic_full("UTSM-PANIC", msg, 0) */
void panic(const char *msg);

#endif /* UTSM_PANIC_H */
