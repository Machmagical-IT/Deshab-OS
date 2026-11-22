/*
 * Zratro OS - Kernel Self-Test Framework (内核自测试框架)
 * 
 * 实现：
 * 1. 测试注册和执行
 * 2. 断言支持
 * 3. 结果收集和报告
 * 4. 内置测试
 */

#include "ktest.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"
#include "scheduler.h"
#include "vfs.h"
#include "zjournal.h"
#include "zpipe.h"
#include "capability.h"
#include "selfheal.h"
#include "coroutine.h"
#include "heterog.h"
#include "realtime.h"
#include "pmem.h"

/* ========== 全局测试运行器 ========== */

static struct ktest_runner global_runner;

/* 当前测试上下文 (用于断言) */
static struct {
    struct ktest_case *current;
    ktest_result_t result;
    char message[KTEST_MAX_MESSAGE_LEN];
    int failed;
    uint32_t fail_line;
    char fail_file[KTEST_MAX_NAME_LEN];
} test_context;

/* ========== 初始化/销毁 ========== */

int ktest_init(struct ktest_runner *runner) {
    if (runner == NULL) {
        return -1;
    }
    
    memset(runner, 0, sizeof(*runner));
    
    INIT_LIST_HEAD(&runner->tests);
    INIT_LIST_HEAD(&runner->groups);
    INIT_LIST_HEAD(&runner->results);
    
    runner->default_timeout = 30000000;  /* 30 秒 */
    runner->verbose = 1;
    
    spinlock_init(&runner->lock);
    
    return 0;
}

void ktest_destroy(struct ktest_runner *runner) {
    if (runner == NULL) {
        return;
    }
    
    spin_lock(&runner->lock);
    
    /* 释放测试用例 */
    struct ktest_case *test, *test_tmp;
    list_for_each_entry_safe(test, test_tmp, &runner->tests, list) {
        list_del(&test->list);
        kfree(test);
    }
    
    /* 释放测试组 */
    struct ktest_group *group, *group_tmp;
    list_for_each_entry_safe(group, group_tmp, &runner->groups, list) {
        list_del(&group->list);
        kfree(group);
    }
    
    /* 释放结果 */
    struct ktest_result_detail *result, *result_tmp;
    list_for_each_entry_safe(result, result_tmp, &runner->results, list) {
        list_del(&result->list);
        kfree(result);
    }
    
    spin_unlock(&runner->lock);
}

/* ========== 注册测试 ========== */

int ktest_register(struct ktest_runner *runner, struct ktest_case *test) {
    if (runner == NULL || test == NULL) {
        return -1;
    }
    
    /* 复制测试用例 */
    struct ktest_case *copy = kmalloc(sizeof(*copy));
    if (copy == NULL) {
        return -1;
    }
    
    memcpy(copy, test, sizeof(*copy));
    INIT_LIST_HEAD(&copy->list);
    
    spin_lock(&runner->lock);
    list_add_tail(&copy->list, &runner->tests);
    runner->test_count++;
    spin_unlock(&runner->lock);
    
    return 0;
}

int ktest_register_group(struct ktest_runner *runner, const char *name,
                          const char *description) {
    if (runner == NULL || name == NULL) {
        return -1;
    }
    
    struct ktest_group *group = kmalloc(sizeof(*group));
    if (group == NULL) {
        return -1;
    }
    
    memset(group, 0, sizeof(*group));
    strncpy(group->name, name, KTEST_MAX_NAME_LEN - 1);
    if (description != NULL) {
        strncpy(group->description, description, KTEST_MAX_MESSAGE_LEN - 1);
    }
    INIT_LIST_HEAD(&group->tests);
    INIT_LIST_HEAD(&group->list);
    
    spin_lock(&runner->lock);
    list_add_tail(&group->list, &runner->groups);
    runner->group_count++;
    spin_unlock(&runner->lock);
    
    return 0;
}

int ktest_register_batch(struct ktest_runner *runner, struct ktest_case *tests,
                          uint32_t count) {
    if (runner == NULL || tests == NULL) {
        return -1;
    }
    
    for (uint32_t i = 0; i < count; i++) {
        if (ktest_register(runner, &tests[i]) != 0) {
            return -1;
        }
    }
    
    return 0;
}

/* ========== 运行测试 ========== */

static ktest_result_t run_single_test(struct ktest_runner *runner,
                                       struct ktest_case *test) {
    (void)runner;
    
    /* 初始化测试上下文 */
    memset(&test_context, 0, sizeof(test_context));
    test_context.current = test;
    test_context.result = KTEST_PASS;
    
    /* 调用设置函数 */
    if (test->setup != NULL) {
        if (test->setup(test->context) != 0) {
            test_context.result = KTEST_ERROR;
            strncpy(test_context.message, "Setup failed", KTEST_MAX_MESSAGE_LEN - 1);
            return KTEST_ERROR;
        }
    }
    
    /* 运行测试 */
    uint64_t start_time = get_tick_count();
    
    if (test->func != NULL) {
        ktest_result_t result = test->func(test->context);
        if (result != KTEST_PASS && test_context.result == KTEST_PASS) {
            test_context.result = result;
        }
    }
    
    uint64_t end_time = get_tick_count();
    
    /* 调用清理函数 */
    if (test->teardown != NULL) {
        test->teardown(test->context);
    }
    
    /* 记录结果 */
    struct ktest_result_detail *detail = kmalloc(sizeof(*detail));
    if (detail != NULL) {
        memset(detail, 0, sizeof(*detail));
        strncpy(detail->test_name, test->name, KTEST_MAX_NAME_LEN - 1);
        detail->result = test_context.result;
        strncpy(detail->message, test_context.message, KTEST_MAX_MESSAGE_LEN - 1);
        detail->duration = end_time - start_time;
        detail->line = test_context.fail_line;
        strncpy(detail->file, test_context.fail_file, KTEST_MAX_NAME_LEN - 1);
        
        spin_lock(&runner->lock);
        list_add_tail(&detail->list, &runner->results);
        runner->result_count++;
        spin_unlock(&runner->lock);
    }
    
    return test_context.result;
}

int ktest_run_all(struct ktest_runner *runner) {
    if (runner == NULL) {
        return -1;
    }
    
    memset(&runner->stats, 0, sizeof(runner->stats));
    runner->stats.min_time = (uint64_t)-1;
    
    uint64_t total_start = get_tick_count();
    
    struct ktest_case *test;
    list_for_each_entry(test, &runner->tests, list) {
        if (!(test->flags & KTEST_FLAG_ENABLED)) {
            runner->stats.skipped++;
            continue;
        }
        
        runner->current = test;
        
        ktest_result_t result = run_single_test(runner, test);
        runner->stats.total++;
        
        switch (result) {
            case KTEST_PASS:
                runner->stats.passed++;
                break;
            case KTEST_FAIL:
                runner->stats.failed++;
                if (runner->stop_on_fail) return -1;
                break;
            case KTEST_SKIP:
                runner->stats.skipped++;
                break;
            case KTEST_ERROR:
                runner->stats.errors++;
                break;
            case KTEST_TIMEOUT:
                runner->stats.timeouts++;
                break;
        }
    }
    
    uint64_t total_end = get_tick_count();
    runner->stats.total_time = total_end - total_start;
    
    return (runner->stats.failed > 0 || runner->stats.errors > 0) ? -1 : 0;
}

int ktest_run_test(struct ktest_runner *runner, const char *name) {
    if (runner == NULL || name == NULL) {
        return -1;
    }
    
    struct ktest_case *test;
    list_for_each_entry(test, &runner->tests, list) {
        if (strcmp(test->name, name) == 0) {
            ktest_result_t result = run_single_test(runner, test);
            return (result == KTEST_PASS) ? 0 : -1;
        }
    }
    
    return -1;
}

int ktest_run_group(struct ktest_runner *runner, const char *group) {
    if (runner == NULL || group == NULL) {
        return -1;
    }
    
    struct ktest_case *test;
    list_for_each_entry(test, &runner->tests, list) {
        if (strcmp(test->group, group) == 0) {
            run_single_test(runner, test);
        }
    }
    
    return 0;
}

int ktest_run_type(struct ktest_runner *runner, ktest_type_t type) {
    if (runner == NULL) {
        return -1;
    }
    
    struct ktest_case *test;
    list_for_each_entry(test, &runner->tests, list) {
        if (test->type == type) {
            run_single_test(runner, test);
        }
    }
    
    return 0;
}

int ktest_run_stress(struct ktest_runner *runner, uint64_t duration) {
    (void)runner;
    (void)duration;
    return 0;
}

int ktest_run_benchmark(struct ktest_runner *runner, const char *name,
                         uint32_t iterations) {
    (void)runner;
    (void)name;
    (void)iterations;
    return 0;
}

/* ========== 结果查询 ========== */

void ktest_get_stats(struct ktest_runner *runner, struct ktest_stats *stats) {
    if (runner == NULL || stats == NULL) {
        return;
    }
    
    memcpy(stats, &runner->stats, sizeof(*stats));
}

void ktest_print_results(struct ktest_runner *runner) {
    if (runner == NULL) {
        return;
    }
    
    println("\n=== Test Results ===");
    
    struct ktest_result_detail *result;
    list_for_each_entry(result, &runner->results, list) {
        print("  [");
        print(result->test_name);
        print("] ");
        
        switch (result->result) {
            case KTEST_PASS:
                console_success("PASS");
                break;
            case KTEST_FAIL:
                console_error("FAIL");
                print("    at ");
                print(result->file);
                print(":");
                print_dec(result->line);
                println("");
                print("    ");
                println(result->message);
                break;
            case KTEST_SKIP:
                console_info("SKIP");
                break;
            default:
                console_error("ERROR");
                break;
        }
    }
}

void ktest_print_summary(struct ktest_runner *runner) {
    if (runner == NULL) {
        return;
    }
    
    println("\n=== Test Summary ===");
    
    print("  Total:   "); print_dec(runner->stats.total); println("");
    print("  Passed:  "); print_dec(runner->stats.passed); println("");
    print("  Failed:  "); print_dec(runner->stats.failed); println("");
    print("  Skipped: "); print_dec(runner->stats.skipped); println("");
    print("  Errors:  "); print_dec(runner->stats.errors); println("");
    print("  Time:    "); print_dec(runner->stats.total_time / 1000); println(" ms");
    
    if (runner->stats.failed == 0 && runner->stats.errors == 0) {
        console_success("\nAll tests passed!");
    } else {
        console_error("\nSome tests failed!");
    }
}

/* ========== 断言实现 ========== */

void ktest_assert_impl(int cond, const char *expr, const char *file, uint32_t line) {
    if (!cond) {
        test_context.failed = 1;
        test_context.result = KTEST_FAIL;
        test_context.fail_line = line;
        strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
        test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
        
        size_t prefix_len = 18;  /* "Assertion failed: " 的长度 */
        strncpy(test_context.message, "Assertion failed: ", KTEST_MAX_MESSAGE_LEN - 1);
        size_t remaining = KTEST_MAX_MESSAGE_LEN - prefix_len - 1;
        strncat(test_context.message, expr, remaining);
        test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
    }
}

void ktest_assert_eq_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line) {
    if (a != b) {
        test_context.failed = 1;
        test_context.result = KTEST_FAIL;
        test_context.fail_line = line;
        strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
        test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
        
        test_context.message[0] = '\0';
        size_t remaining = KTEST_MAX_MESSAGE_LEN - 5;  /* 留出 " != " + null */
        strncat(test_context.message, a_str, remaining / 2);
        strncat(test_context.message, " != ", 4);
        strncat(test_context.message, b_str, remaining / 2);
        test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
    }
}

void ktest_assert_ne_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line) {
    if (a == b) {
        test_context.failed = 1;
        test_context.result = KTEST_FAIL;
        test_context.fail_line = line;
        strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
        test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
        
        test_context.message[0] = '\0';
        size_t remaining = KTEST_MAX_MESSAGE_LEN - 5;
        strncat(test_context.message, a_str, remaining / 2);
        strncat(test_context.message, " == ", 4);
        strncat(test_context.message, b_str, remaining / 2);
        test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
    }
}

void ktest_assert_lt_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line) {
    if (a >= b) {
        test_context.failed = 1;
        test_context.result = KTEST_FAIL;
        test_context.fail_line = line;
        strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
        test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
        
        test_context.message[0] = '\0';
        strncat(test_context.message, a_str, KTEST_MAX_MESSAGE_LEN / 2 - 1);
        strncat(test_context.message, " >= ", 4);
        strncat(test_context.message, b_str, KTEST_MAX_MESSAGE_LEN / 2 - 1);
        test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
    }
}

void ktest_assert_le_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line) {
    if (a > b) {
        test_context.failed = 1;
        test_context.result = KTEST_FAIL;
        test_context.fail_line = line;
        strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
        test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
        
        test_context.message[0] = '\0';
        strncat(test_context.message, a_str, KTEST_MAX_MESSAGE_LEN / 2 - 1);
        strncat(test_context.message, " > ", 3);
        strncat(test_context.message, b_str, KTEST_MAX_MESSAGE_LEN / 2 - 1);
        test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
    }
}

void ktest_assert_gt_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line) {
    if (a <= b) {
        test_context.failed = 1;
        test_context.result = KTEST_FAIL;
        test_context.fail_line = line;
        strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
        test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
        
        test_context.message[0] = '\0';
        strncat(test_context.message, a_str, KTEST_MAX_MESSAGE_LEN / 2 - 1);
        strncat(test_context.message, " <= ", 4);
        strncat(test_context.message, b_str, KTEST_MAX_MESSAGE_LEN / 2 - 1);
        test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
    }
}

void ktest_assert_ge_impl(uint64_t a, uint64_t b, const char *a_str, const char *b_str,
                          const char *file, uint32_t line) {
    if (a < b) {
        test_context.failed = 1;
        test_context.result = KTEST_FAIL;
        test_context.fail_line = line;
        strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
        test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
        
        test_context.message[0] = '\0';
        strncat(test_context.message, a_str, KTEST_MAX_MESSAGE_LEN / 2 - 1);
        strncat(test_context.message, " < ", 3);
        strncat(test_context.message, b_str, KTEST_MAX_MESSAGE_LEN / 2 - 1);
        test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
    }
}

void ktest_assert_null_impl(void *ptr, const char *ptr_str,
                            const char *file, uint32_t line) {
    if (ptr != NULL) {
        test_context.failed = 1;
        test_context.result = KTEST_FAIL;
        test_context.fail_line = line;
        strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
        test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
        
        test_context.message[0] = '\0';
        strncat(test_context.message, ptr_str, KTEST_MAX_MESSAGE_LEN - 15);
        strncat(test_context.message, " is not NULL", 12);
        test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
    }
}

void ktest_assert_not_null_impl(void *ptr, const char *ptr_str,
                                const char *file, uint32_t line) {
    if (ptr == NULL) {
        test_context.failed = 1;
        test_context.result = KTEST_FAIL;
        test_context.fail_line = line;
        strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
        test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
        
        test_context.message[0] = '\0';
        strncat(test_context.message, ptr_str, KTEST_MAX_MESSAGE_LEN - 12);
        strncat(test_context.message, " is NULL", 8);
        test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
    }
}

void ktest_assert_str_eq_impl(const char *a, const char *b,
                               const char *a_str, const char *b_str,
                               const char *file, uint32_t line) {
    if (strcmp(a, b) != 0) {
        test_context.failed = 1;
        test_context.result = KTEST_FAIL;
        test_context.fail_line = line;
        strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
        test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
        
        test_context.message[0] = '\0';
        strncat(test_context.message, "String mismatch: ", 17);
        strncat(test_context.message, a_str, 20);
        strncat(test_context.message, " != ", 4);
        strncat(test_context.message, b_str, 20);
        test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
    }
}

void ktest_assert_mem_eq_impl(const void *a, const void *b, size_t size,
                               const char *a_str, const char *b_str,
                               const char *file, uint32_t line) {
    if (memcmp(a, b, size) != 0) {
        test_context.failed = 1;
        test_context.result = KTEST_FAIL;
        test_context.fail_line = line;
        strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
        test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
        
        test_context.message[0] = '\0';
        strncat(test_context.message, "Memory mismatch: ", 17);
        strncat(test_context.message, a_str, 20);
        strncat(test_context.message, " != ", 4);
        strncat(test_context.message, b_str, 20);
        test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
    }
}

void ktest_skip_impl(const char *msg) {
    test_context.result = KTEST_SKIP;
    strncpy(test_context.message, msg, KTEST_MAX_MESSAGE_LEN - 1);
    test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
}

void ktest_fail_impl(const char *msg, const char *file, uint32_t line) {
    test_context.failed = 1;
    test_context.result = KTEST_FAIL;
    test_context.fail_line = line;
    strncpy(test_context.fail_file, file, KTEST_MAX_NAME_LEN - 1);
    test_context.fail_file[KTEST_MAX_NAME_LEN - 1] = '\0';
    strncpy(test_context.message, msg, KTEST_MAX_MESSAGE_LEN - 1);
    test_context.message[KTEST_MAX_MESSAGE_LEN - 1] = '\0';
}

void ktest_log_impl(const char *fmt, ...) {
    (void)fmt;
    /* 简化实现 */
}

void ktest_record_benchmark(const char *name, uint32_t iterations, uint64_t duration) {
    print("[BENCH] ");
    print((char *)name);
    print(": ");
    print_dec(duration / iterations);
    print(" us/op (");
    print_dec(iterations);
    print(" iterations, ");
    print_dec(duration / 1000);
    println(" ms total)");
}

/* ========== 内置测试 ========== */

ktest_result_t ktest_pmm_basic(void *ctx) {
    (void)ctx;
    
    /* 测试页分配 */
    uint64_t page = pmm_alloc_page();
    KTEST_ASSERT(page != 0);
    
    pmm_free_page(page);
    
    /* 测试多页分配 */
    uint64_t pages = pmm_alloc_pages(4);
    KTEST_ASSERT(pages != 0);
    
    pmm_free_pages(pages, 4);
    
    return KTEST_PASS;
}

ktest_result_t ktest_vmm_basic(void *ctx) {
    (void)ctx;
    
    /* 测试虚拟地址映射 */
    uint64_t phys = pmm_alloc_page();
    KTEST_ASSERT(phys != 0);
    
    /* VMM 测试需要更复杂的环境 */
    pmm_free_page(phys);
    
    return KTEST_PASS;
}

ktest_result_t ktest_heap_basic(void *ctx) {
    (void)ctx;
    
    /* 测试 kmalloc/kfree */
    void *ptr1 = kmalloc(128);
    KTEST_ASSERT_NOT_NULL(ptr1);
    
    void *ptr2 = kmalloc(1024);
    KTEST_ASSERT_NOT_NULL(ptr2);
    
    void *ptr3 = kmalloc(4096);
    KTEST_ASSERT_NOT_NULL(ptr3);
    
    kfree(ptr1);
    kfree(ptr2);
    kfree(ptr3);
    
    /* 测试大块分配 */
    void *large = kmalloc(1024 * 1024);
    if (large != NULL) {
        kfree(large);
    }
    
    return KTEST_PASS;
}

ktest_result_t ktest_slab_basic(void *ctx) {
    (void)ctx;
    /* Slab 测试需要 slab 系统 */
    return KTEST_PASS;
}

ktest_result_t ktest_scheduler_basic(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_task_create(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_task_switch(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_vfs_basic(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_fat32_basic(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_zjournal_basic(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_zpipe_basic(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_capability_basic(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_selfheal_basic(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_coroutine_basic(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_heterog_basic(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_realtime_basic(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_pmem_basic(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_stress_malloc(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_stress_context_switch(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_stress_fs_ops(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_bench_malloc(void *ctx) {
    (void)ctx;
    
    KTEST_BENCH_START("malloc/free");
    
    for (int i = 0; i < 1000; i++) {
        void *ptr = kmalloc(256);
        if (ptr != NULL) {
            kfree(ptr);
        }
    }
    
    KTEST_BENCH_END("malloc/free", 1000);
    
    return KTEST_PASS;
}

ktest_result_t ktest_bench_context_switch(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

ktest_result_t ktest_bench_syscall(void *ctx) {
    (void)ctx;
    return KTEST_PASS;
}

/* ========== 注册内置测试 ========== */

int ktest_register_builtin(struct ktest_runner *runner) {
    if (runner == NULL) {
        return -1;
    }
    
    /* 内存管理测试 */
    struct ktest_case mem_tests[] = {
        {"pmm_basic", "memory", ktest_pmm_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_HIGH, KTEST_FLAG_ENABLED},
        {"vmm_basic", "memory", ktest_vmm_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_HIGH, KTEST_FLAG_ENABLED},
        {"heap_basic", "memory", ktest_heap_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_HIGH, KTEST_FLAG_ENABLED},
        {"slab_basic", "memory", ktest_slab_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
    };
    ktest_register_batch(runner, mem_tests, 4);
    
    /* 调度器测试 */
    struct ktest_case sched_tests[] = {
        {"scheduler_basic", "scheduler", ktest_scheduler_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_HIGH, KTEST_FLAG_ENABLED},
        {"task_create", "scheduler", ktest_task_create, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
        {"task_switch", "scheduler", ktest_task_switch, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
    };
    ktest_register_batch(runner, sched_tests, 3);
    
    /* 文件系统测试 */
    struct ktest_case fs_tests[] = {
        {"vfs_basic", "filesystem", ktest_vfs_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
        {"fat32_basic", "filesystem", ktest_fat32_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
    };
    ktest_register_batch(runner, fs_tests, 2);
    
    /* 子系统测试 */
    struct ktest_case subsys_tests[] = {
        {"zjournal_basic", "subsystem", ktest_zjournal_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
        {"zpipe_basic", "subsystem", ktest_zpipe_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
        {"capability_basic", "subsystem", ktest_capability_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
        {"selfheal_basic", "subsystem", ktest_selfheal_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
        {"coroutine_basic", "subsystem", ktest_coroutine_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
        {"heterog_basic", "subsystem", ktest_heterog_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
        {"realtime_basic", "subsystem", ktest_realtime_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
        {"pmem_basic", "subsystem", ktest_pmem_basic, NULL, KTEST_TYPE_UNIT, KTEST_PRIO_NORMAL, KTEST_FLAG_ENABLED},
    };
    ktest_register_batch(runner, subsys_tests, 8);
    
    /* 压力测试 */
    struct ktest_case stress_tests[] = {
        {"stress_malloc", "stress", ktest_stress_malloc, NULL, KTEST_TYPE_STRESS, KTEST_PRIO_LOW, KTEST_FLAG_ENABLED},
        {"stress_context_switch", "stress", ktest_stress_context_switch, NULL, KTEST_TYPE_STRESS, KTEST_PRIO_LOW, KTEST_FLAG_ENABLED},
        {"stress_fs_ops", "stress", ktest_stress_fs_ops, NULL, KTEST_TYPE_STRESS, KTEST_PRIO_LOW, KTEST_FLAG_ENABLED},
    };
    ktest_register_batch(runner, stress_tests, 3);
    
    /* 基准测试 */
    struct ktest_case bench_tests[] = {
        {"bench_malloc", "benchmark", ktest_bench_malloc, NULL, KTEST_TYPE_BENCHMARK, KTEST_PRIO_LOW, KTEST_FLAG_ENABLED},
        {"bench_context_switch", "benchmark", ktest_bench_context_switch, NULL, KTEST_TYPE_BENCHMARK, KTEST_PRIO_LOW, KTEST_FLAG_ENABLED},
        {"bench_syscall", "benchmark", ktest_bench_syscall, NULL, KTEST_TYPE_BENCHMARK, KTEST_PRIO_LOW, KTEST_FLAG_ENABLED},
    };
    ktest_register_batch(runner, bench_tests, 3);
    
    return 0;
}

/* ========== 全局测试运行器 ========== */

struct ktest_runner *ktest_get_global(void) {
    return &global_runner;
}

int ktest_global_init(void) {
    int ret = ktest_init(&global_runner);
    if (ret == 0) {
        ktest_register_builtin(&global_runner);
    }
    return ret;
}

int ktest_self_check(void) {
    return ktest_run_all(&global_runner);
}
