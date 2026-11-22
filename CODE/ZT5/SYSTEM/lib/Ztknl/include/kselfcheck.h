/**
 * @file kselfcheck.h
 * @brief 内核自检机制
 * 
 * 在启动时执行关键子系统验证，确保内核处于健康状态。
 */

#ifndef KSELFCHECK_H
#define KSELFCHECK_H

#include <stdint.h>

/* 自检结果 */
#define SELFCHECK_PASS  0   /* 通过 */
#define SELFCHECK_WARN  1   /* 警告 */
#define SELFCHECK_FAIL  2   /* 失败 */

/* 自检记录 */
struct selfcheck_record {
    char name[64];          /* 检查项名称 */
    int result;             /* 结果 */
    char detail[128];       /* 详细信息 */
};

/**
 * @brief 执行完整的内核自检
 * @return 通过的检查数量
 */
int kselfcheck_run_all(void);

/**
 * @brief 快速健康检查
 * @return 0 表示健康，非 0 表示有问题
 */
int kselfcheck_quick(void);

/**
 * @brief 获取自检统计
 */
void kselfcheck_get_stats(int *total, int *passed, int *failed, int *warnings);

/**
 * @brief 获取最后一次检查时间
 */
uint64_t kselfcheck_get_last_time(void);

/**
 * @brief 检查系统是否健康
 */
int kselfcheck_is_healthy(void);

/* ========== 便捷宏 ========== */

/**
 * 快速健康检查断言
 */
#define SELFCHECK_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            println("[SELFCHECK] Assertion failed: " #cond); \
        } \
    } while(0)

#endif /* KSELFCHECK_H */
