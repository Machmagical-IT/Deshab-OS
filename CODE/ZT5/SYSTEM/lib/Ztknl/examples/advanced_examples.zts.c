/*
 * advanced_examples.zts.c - 高级 .zts 模块示例
 * 
 * 展示三种模式：
 *   1. API 模式 - 通过稳定 API 与内核交互
 *   2. 补丁模式 - 直接修改内核代码
 *   3. 注入模式 - 插入代码到指定位置执行
 */

#include <stdint.h>
#include <stddef.h>
#include "zts_algorithm.h"

/* ============================================================
 * 示例 1: API 模式 - 加密算法
 * 文件名: 0x1000a001.crypto_aes.zts
 * UUID: {0x12, 0x34, 0x56, 0x78, ...}
 */

static uint8_t aes_uuid[ZTS_UUID_LEN] = {
    0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
};

ZTS_DECLARE(
    "crypto_aes",
    "AES-256 加密算法模块",
    "ZratroOS Team",
    "GPL",
    ZTS_CAT_CRYPTO,
    2, 0
);

/* 覆盖 UUID */
static void __attribute__((constructor)) set_uuid(void) {
    __zts_header.uuid = aes_uuid;
}

struct aes_ctx {
    uint8_t key[32];
    uint8_t iv[16];
    int rounds;
};

ZTS_MODULE_INIT(aes_init) {
    struct aes_ctx *ctx = zts_alloc(sizeof(*ctx));
    if (!ctx) return -1;
    
    ctx->rounds = 14;  /* AES-256 */
    alg->data = ctx;
    alg->data_size = sizeof(*ctx);
    
    zts_info("AES-256 initialized\n");
    return 0;
}

ZTS_MODULE_EXECUTE(aes_encrypt) {
    struct aes_ctx *ctx = (struct aes_ctx *)alg->data;
    
    /* 简化的 AES 加密示例 */
    zts_info("Encrypting data with AES-256 (%d rounds)\n", ctx->rounds);
    
    /* 实际加密逻辑... */
    return 0;
}

ZTS_MODULE_CLEANUP(aes_cleanup) {
    struct aes_ctx *ctx = (struct aes_ctx *)alg->data;
    if (ctx) {
        /* 清除敏感数据 */
        __builtin_memset(ctx, 0, sizeof(*ctx));
        zts_free(ctx);
    }
    zts_info("AES cleaned up\n");
}

/* ============================================================
 * 示例 2: 补丁模式 - 修改内核函数
 * 文件名: 0x2000b002.patch_scheduler.zts
 * UUID: {0xAA, 0xBB, 0xCC, 0xDD, ...}
 * 
 * 这个模块会直接修改内核调度器的代码
 */

static uint8_t patch_uuid[ZTS_UUID_LEN] = {
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
    0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99
};

/* 补丁数据 - 新的调度算法代码 */
static uint8_t new_scheduler_code[] = {
    /* x86_64 机器码示例 */
    0x55,                   /* push rbp */
    0x48, 0x89, 0xE5,       /* mov rbp, rsp */
    /* ... 新的调度逻辑 ... */
    0x5D,                   /* pop rbp */
    0xC3                    /* ret */
};

ZTS_MODULE_INIT(patch_init) {
    zts_info("Scheduler patch module loaded\n");
    zts_info("Target address: 0x%lx\n", alg->patch_address);
    zts_info("Patch size: %lu bytes\n", alg->patch_size);
    return 0;
}

ZTS_MODULE_CLEANUP(patch_cleanup) {
    /* 恢复原始代码 */
    zts_restore_patch(alg);
    zts_info("Scheduler patch restored\n");
}

/* ============================================================
 * 示例 3: 注入模式 - 在指定位置插入钩子
 * 文件名: 0x3000c003.hook_syscall.zts
 * UUID: {0x11, 0x22, 0x33, 0x44, ...}
 * 
 * 这个模块会在系统调用入口点插入钩子代码
 */

static uint8_t hook_uuid[ZTS_UUID_LEN] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00
};

/* 钩子代码 - 记录系统调用 */
static uint8_t syscall_hook_code[] = {
    /* 保存寄存器 */
    0x50,                   /* push rax */
    0x51,                   /* push rcx */
    0x52,                   /* push rdx */
    /* ... 记录系统调用号 (在 rax 中) ... */
    /* 恢复寄存器 */
    0x5A,                   /* pop rdx */
    0x59,                   /* pop rcx */
    0x58,                   /* pop rax */
    /* 跳转回原代码 */
    0xE9, 0x00, 0x00, 0x00, 0x00  /* jmp rel32 (需要计算偏移) */
};

ZTS_MODULE_INIT(hook_init) {
    zts_info("Syscall hook module loaded\n");
    zts_info("Hook address: 0x%lx\n", alg->patch_address);
    return 0;
}

ZTS_MODULE_EXECUTE(hook_execute) {
    /* 钩子执行逻辑 */
    zts_info("Syscall hook triggered\n");
    return 0;
}

ZTS_MODULE_CLEANUP(hook_cleanup) {
    zts_info("Syscall hook removed\n");
}
