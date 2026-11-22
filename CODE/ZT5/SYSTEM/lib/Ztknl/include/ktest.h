/*
 * Zratro OS - Kernel Self-Test Framework (内核自测试框架)
 * 
 * 提供：
 * 1. 单元测试 - 测试单个函数/模块
 * 2. 集成测试 - 测试模块间交互
 * 3. 压力测试 - 高负载场景测试
 * 4. 基准测试 - 性能基准
 * 5. 回归测试 - 确保修改不破坏现有功能
 */

#ifndef KTEST_H
#define KTEST_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== 测试配置 ========== */

#define KTEST_MAX_TESTS         128         /* 最大测试数 */
#define KTEST_MAX_NAME_LEN      48          /* 名字最大长度 */
#define KTEST_MAX_MESSAGE_LEN   256         /* 消息最大长度 */
#define KTEST_MAX_GROUPS        16          /* 最大测试组数 */

/* ========== 测试结果 ========== */

typedef enum {
    KTEST_PASS = 0,
    KTEST_FAIL,
    KTEST_SKIP,
    KTEST_ERROR,
    KTEST_TIMEOUT,
} ktest_result_t;

/* ========== 测试类型 ========== */

typedef enum {
    KTEST_TYPE_UNIT = 0,        /* 单元测试 */
    KTEST_TYPE_INTEGRATION,     /* 集成测试 */
    KTEST_TYPE_STRESS,          /* 压力测试 */
    KTEST_TYPE_BENCHMARK,       /* 基准测试 */
    KTEST_TYPE_REGRESSION,      /* 回归测试 */
    KTEST_TYPE_SECURITY,        /* 安全测试 */
} ktest_type_t;

/* ========== 测试优先级 ========== */

typedef enum {
    KTEST_PRIO_LOW = 0,
    KTEST_PRIO_NORMAL,
    KTEST_PRIO_HIGH,
    KTEST_PRIO_CRITICAL,
} ktest_priority_t;

/* ========== 测试统计 ========== */

struct ktest_stats {
    uint32_t total;
    uint32_t passed;
    uint32_t failed;
    uint32_t skipped;
    uint32_t errors;
    uint32_t timeouts;
    
    uint64_t total_time;        /* 总耗时 */
    uint64_t min_time;          /* 最小耗时 */
    uint64_t max_time;          /* 最大耗时 */
    uint64_t avg_time;          /* 平均耗时 */
};

/* ========== 测试结果详情 ========== */

struct ktest_result_detail {
    char test_name[KTEST_MAX_NAME_LEN];
    ktest_result_t result;
    char message[KTEST_MAX_MESSAGE_LEN];
    uint64_t duration;          /* 耗时 */
    uint32_t line;              /* 失败行号 */
    char file[KTEST_MAX_NAME_LEN]; /* 失败文件 */
    
    struct list_head list;
};

/* ========== 测试用例 ========== */

typedef ktest_result_t (*ktest_func_t)(void *context);

struct ktest_case {
    char name[KTEST_MAX_NAME_LEN];
    char group[KTEST_MAX_NAME_LEN];
    ktest_func_t func;
    void *context;
    
    ktest_type_t type;
    ktest_priority_t priority;
    uint32_t flags;
    
    /* 超时设置 */
    uint64_t timeout;           /* 超时时间 */
    
    /* 设置/清理函数 */
    int (*setup)(void *context);
    void (*teardown)(void *context);
    
    /* 基准测试参数 */
    uint32_t iterations;        /* 迭代次数 */
    uint32_t warmup;            /* 预热次数 */
    
    struct list_head list;
};

/* 测试标志 */
#define KTEST_FLAG_ENABLED      (1 << 0)
#define KTEST_FLAG_PARALLEL     (1 << 1)  /* 可并行执行 */
#define KTEST_FLAG_VERBOSE      (1 << 2)  /* 详细输出 */
#define KTEST_FLAG_ISOLATED     (1 << 3)  /* 隔离执行 */

/* ========== 测试组 ========== */

struct ktest_group {
    char name[KTEST_MAX_NAME_LEN];
    char description[KTEST_MAX_MESSAGE_LEN];
    
    struct list_head tests;
    uint32_t test_count;
    
    struct ktest_stats stats;
    
    struct list_head list;
};

/* ========== 测试运行器 ========== */

struct ktest_runner {
    /* 测试用例 */
    struct list_head tests;
    uint32_t test_count;
    
    /* 测试组 */
    struct list_head groups;
    uint32_t group_count;
    
    /* 结果 */
    struct list_head results;
    uint32_t result_count;
    
    /* 统计 */
    struct ktest_stats stats;
    
    /* 配置 */
    uint32_t flags;
    uint64_t default_timeout;
    int stop_on_fail;
    int verbose;
    
    /* 当前测试 */
    struct ktest_case *current;
    
    /* 锁 */
    spinlock_t lock;
};

/* 运行器标志 */
#define KTEST_RUNNER_PARALLEL   (1 << 0)
#define KTEST_RUNNER_VERBOSE    (1 << 1)
#define KTEST_RUNNER_BAIL_OUT   (1 << 2)  /* 首次失败停止 */

/* ========== 断言宏 ========== */

#define KTEST_ASSERT(cond) \
    ktest_assert_impl((cond), #cond, __FILE__, __LINE__)

#define KTEST_ASSERT_EQ(a, b) \
    ktest_assert_eq_impl((uint64_t)(a), (uint64_t)(b), #a, #b, __FILE__, __LINE__)

#define KTEST_ASSERT_NE(a, b) \
    ktest_assert_ne_impl((uint64_t)(a), (uint64_t)(b), #a, #b, __FILE__, __LINE__)

#define KTEST_ASSERT_LT(a, b) \
    ktest_assert_lt_impl((uint64_t)(a), (uint64_t)(b), #a, #b, __FILE__, __LINE__)

#define KTEST_ASSERT_LE(a, b) \
    ktest_assert_le_impl((uint64_t)(a), (uint64_t)(b), #a, #b, __FILE__, __LINE__)

#define KTEST_ASSERT_GT(a, b) \
    ktest_assert_gt_impl((uint64_t)(a), (uint64_t)(b), #a, #b, __FILE__, __LINE__)

#define KTEST_ASSERT_GE(a, b) \
    ktest_assert_ge_impl((uint64_t)(a), (uint64_t)(b), #a, #b, __FILE__, __LINE__)

#define KTEST_ASSERT_NULL(ptr) \
    ktest_assert_null_impl((ptr), #ptr, __FILE__, __LINE__)

#define KTEST_ASSERT_NOT_NULL(ptr) \
    ktest_assert_not_null_impl((ptr), #ptr, __FILE__, __LINE__)

#define KTEST_ASSERT_STR_EQ(a, b) \
    ktest_assert_str_eq_impl((a), (b), #a, #b, __FILE__, __LINE__)

#define KTEST_ASSERT_MEM_EQ(a, b, size) \
    ktest_assert_mem_eq_impl((a), (b), (size), #a, #b, __FILE__, __LINE__)

#define KTEST_SKIP(msg) \
    do { ktest_skip_impl(msg); return KTEST_SKIP; } while(0)

#define KTEST_FAIL(msg) \
    do { ktest_fail_impl(msg, __FILE__, __LINE__); return KTEST_FAIL; } while(0)

#define KTEST_LOG(fmt, ...) \
    ktest_log_impl(fmt, ##__VA_ARGS__)

/* ========== 基准测试宏 ========== */

#define KTEST_BENCH_START(name) \
    uint64_t __bench_start = get_tick_count()

#define KTEST_BENCH_END(name, iterations) \
    do { \
        uint64_t __bench_end = get_tick_count(); \
        uint64_t __bench_duration = __bench_end - __bench_start; \
        ktest_record_benchmark(name, iterations, __bench_duration); \
    } while(0)

/* ========== 公共 API ========== */

/* 初始化 */
int ktest_init(struct ktest_runner *runner);
void ktest_destroy(struct ktest_runner *runner);

/* ========== 注册测试 ========== */

/* 注册测试用例 */
int ktest_register(struct ktest_runner *runner, struct ktest_case *test);

/* 注册测试组 */
int ktest_register_group(struct ktest_runner *runner, const char *name,
                          const char *description);

/* 批量注册测试 */
int ktest_register_batch(struct ktest_runner *runner, struct ktest_case *tests,
                          uint32_t count);

/* ========== 运行测试 ========== */

/* 运行所有测试 */
int ktest_run_all(struct ktest_runner *runner);

/* 运行指定测试 */
int ktest_run_test(struct ktest_runner *runner, const char *name);

/* 运行指定组 */
int ktest_run_group(struct ktest_runner *runner, const char *group);

/* 运行指定类型 */
int ktest_run_type(struct ktest_runner *runner, ktest_type_t type);

/* 运行压力测试 */
int ktest_run_stress(struct ktest_runner *runner, uint64_t duration);

/* 运行基准测试 */
int ktest_run_benchmark(struct ktest_runner *runner, const char *name,
                         uint32_t iterations);

/* ========== 结果查询 ========== */

/* 获取统计 */
void ktest_get_stats(struct ktest_runner *runner, struct ktest_stats *stats);

/* 获取结果列表 */
struct ktest_result_detail *ktest_get_results(struct ktest_runner *runner,
                                               uint32_t *count);

/* 打印结果 */
void ktest_print_results(struct ktest_runner *runner);

/* 打印汇总 */
void ktest_print_summary(struct ktest_runner *runner);

/* ========== 断言实现 ========== */

void ktest_assert_impl(int cond, const char *expr, const char *file, uint32_t line);
void ktest_assert_eq_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line);
void ktest_assert_ne_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line);
void ktest_assert_lt_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line);
void ktest_assert_le_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line);
void ktest_assert_gt_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line);
void ktest_assert_ge_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line);
void ktest_assert_null_impl(void *ptr, const char *ptr_str,
                            const char *file, uint32_t line);
void ktest_assert_not_null_impl(void *ptr, const char *ptr_str,
                                const char *file, uint32_t line);
void ktest_assert_str_eq_impl(const char *a, const char *b,
                               const char *a_str, const char *b_str,
                               const char *file, uint32_t line);
void ktest_assert_mem_eq_impl(const void *a, const void *b, size_t size,
                               const char *a_str, const char *b_str,
                               const char *file, uint32_t line);

void ktest_skip_impl(const char *msg);
void ktest_fail_impl(const char *msg, const char *file, uint32_t line);
void ktest_log_impl(const char *fmt, ...);
void ktest_record_benchmark(const char *name, uint32_t iterations, uint64_t duration);

/* ========== 内置测试 ========== */

/* 内存管理测试 */
ktest_result_t ktest_pmm_basic(void *ctx);
ktest_result_t ktest_vmm_basic(void *ctx);
ktest_result_t ktest_heap_basic(void *ctx);
ktest_result_t ktest_slab_basic(void *ctx);

/* 调度器测试 */
ktest_result_t ktest_scheduler_basic(void *ctx);
ktest_result_t ktest_task_create(void *ctx);
ktest_result_t ktest_task_switch(void *ctx);

/* 文件系统测试 */
ktest_result_t ktest_vfs_basic(void *ctx);
ktest_result_t ktest_fat32_basic(void *ctx);

/* 子系统测试 */
ktest_result_t ktest_zjournal_basic(void *ctx);
ktest_result_t ktest_zpipe_basic(void *ctx);
ktest_result_t ktest_capability_basic(void *ctx);
ktest_result_t ktest_selfheal_basic(void *ctx);
ktest_result_t ktest_coroutine_basic(void *ctx);
ktest_result_t ktest_heterog_basic(void *ctx);
ktest_result_t ktest_realtime_basic(void *ctx);
ktest_result_t ktest_pmem_basic(void *ctx);

/* 压力测试 */
ktest_result_t ktest_stress_malloc(void *ctx);
ktest_result_t ktest_stress_context_switch(void *ctx);
ktest_result_t ktest_stress_fs_ops(void *ctx);

/* 基准测试 */
ktest_result_t ktest_bench_malloc(void *ctx);
ktest_result_t ktest_bench_context_switch(void *ctx);
ktest_result_t ktest_bench_syscall(void *ctx);

/* 注册所有内置测试 */
int ktest_register_builtin(struct ktest_runner *runner);

/* ========== 全局测试运行器 ========== */

struct ktest_runner *ktest_get_global(void);
int ktest_global_init(void);

/* 运行内核自检 */
int ktest_self_check(void);

#endif /* KTEST_H */
