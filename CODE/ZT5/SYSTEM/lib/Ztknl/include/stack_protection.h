/**
 * @file stack_protection.h
 * @brief 内核栈保护机制
 *
 * 提供栈 canary 支持，防止栈缓冲区溢出攻击。
 * 支持 CET Shadow Stack、Canary 验证、栈回溯。
 */

#ifndef STACK_PROTECTION_H
#define STACK_PROTECTION_H

#include <stdint.h>

/* 默认栈 canary 值 (应在启动时随机化) */
#define STACK_CANARY_VALUE 0xDEADBEEFCAFEBABEULL

/* 全局栈 canary - 编译器引用此符号 */
extern uint64_t __stack_chk_guard;

/**
 * @brief 初始化栈保护
 *
 * 使用 TSC 作为随机源初始化全局 canary 值。
 * 必须在启动早期调用。
 */
void stack_protection_init(void);

/**
 * @brief 为进程分配栈 canary
 * @return 进程专用的 canary 值
 */
uint64_t stack_canary_alloc(void);

/**
 * @brief 释放进程栈 canary
 * @param canary 要释放的 canary 值
 */
void stack_canary_free(uint64_t canary);

/**
 * @brief 验证栈 canary
 * @param canary 要验证的 canary 值
 * @return 1 表示有效，0 表示无效
 */
int stack_canary_verify(uint64_t canary);

/**
 * @brief Stack canary 设置函数
 *
 * 由编译器在函数入口处调用。
 */
void __stack_chk_setup(void);

/**
 * @brief Stack canary 失败处理函数
 *
 * 当检测到栈缓冲区溢出时由编译器调用。
 * 此函数不会返回。
 */
__attribute__((noreturn))
void __stack_chk_fail(void);

/**
 * @brief Stack canary 失败处理函数 (本地版本)
 */
__attribute__((noreturn))
void __stack_chk_fail_local(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * 栈保护辅助宏
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * 在关键函数中手动设置栈 canary
 */
#define STACK_CANARY_SETUP() \
    uint64_t __local_canary = __stack_chk_guard; \
    volatile uint64_t __stack_cookie = __local_canary

/**
 * 在函数退出前检查栈 canary
 */
#define STACK_CANARY_CHECK() \
    do { \
        if (__stack_cookie != __local_canary) { \
            __stack_chk_fail(); \
        } \
    } while(0)

/**
 * 在关键代码块前后添加栈保护
 */
#define STACK_PROTECTED_BLOCK(name, code) \
    do { \
        STACK_CANARY_SETUP(); \
        code; \
        STACK_CANARY_CHECK(); \
    } while(0)

/* ═══════════════════════════════════════════════════════════════════════════
 * CET Shadow Stack 支持
 * 使用 Intel CET (Control-flow Enforcement Technology) 的 Shadow Stack
 * 来检测返回地址篡改
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Shadow Stack 配置 */
#define SHADOW_STACK_SIZE       0x4000  /* 16KB Shadow Stack */

/* 检查 CPU 是否支持 CET Shadow Stack */
int stack_cet_supported(void);

/* 初始化 Shadow Stack (每个 CPU 调用一次) */
int stack_shadow_stack_init(void);

/* 启用 Shadow Stack */
int stack_shadow_stack_enable(void);

/* 禁用 Shadow Stack */
void stack_shadow_stack_disable(void);

/* 检查 Shadow Stack 是否已启用 */
int stack_shadow_stack_is_enabled(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * Stack Canary 增强验证
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 在函数返回时验证 canary (由编译器或手动调用) */
int stack_canary_check_on_return(uint64_t saved_canary, uint64_t expected_canary);

/* 获取当前线程的 canary 值 */
uint64_t stack_get_current_canary(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * 栈回溯 (Backtrace) 工具
 * 遍历栈帧用于调试和错误报告
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 栈帧结构 (x86-64 标准帧指针布局) */
struct stack_frame {
    struct stack_frame *next;    /* 上一级帧指针 (RBP) */
    uint64_t return_addr;       /* 返回地址 (RIP) */
};

/* 回溯结果 */
#define STACK_TRACE_MAX     64

struct stack_trace {
    uint64_t frames[STACK_TRACE_MAX];   /* 返回地址数组 */
    int count;                          /* 实际帧数 */
};

/**
 * @brief 执行栈回溯
 *
 * 从当前 RBP 开始遍历栈帧链，收集返回地址
 *
 * @param trace 输出的栈回溯结果
 * @param max_frames 最大收集帧数
 * @return 实际收集的帧数
 */
int stack_backtrace(struct stack_trace *trace, int max_frames);

/**
 * @brief 从指定帧指针执行栈回溯
 *
 * @param trace 输出的栈回溯结果
 * @param rbp 起始帧指针
 * @param max_frames 最大收集帧数
 * @return 实际收集的帧数
 */
int stack_backtrace_from(struct stack_trace *trace, uint64_t rbp, int max_frames);

/**
 * @brief 打印栈回溯
 *
 * @param trace 栈回溯结果
 */
void stack_print_backtrace(struct stack_trace *trace);

#endif /* STACK_PROTECTION_H */
