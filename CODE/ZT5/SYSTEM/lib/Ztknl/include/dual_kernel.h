/*
 * Zratro OS - Dual Kernel Manager
 * 
 * 管理两个同时运行的内核（Ztknl 和 Azknl）
 */

#ifndef DUAL_KERNEL_H
#define DUAL_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include "vmx.h"
#include "inter_kernel.h"

/* ========== 双内核状态 ========== */

typedef enum {
    DK_STATE_INIT = 0,       /* 初始化中 */
    DK_STATE_LOADING,        /* 加载中 */
    DK_STATE_RUNNING,        /* 运行中 */
    DK_STATE_SUSPENDED,      /* 已挂起 */
    DK_STATE_ERROR,          /* 错误状态 */
} dual_kernel_state_t;

/* ========== 双内核管理器 ========== */

struct dual_kernel_manager {
    /* 状态 */
    dual_kernel_state_t state;
    int is_primary;          /* 是否为主内核 */
    
    /* 共享内存 */
    struct inter_kernel_shm *shm;
    
    /* 副内核虚拟机 */
    struct vm_state secondary_vm;
    
    /* 主机栈（用于 VM-Exit） */
    uint8_t host_stack[8192] __attribute__((aligned(16)));
    
    /* 统计信息 */
    uint64_t vm_exits;
    uint64_t messages_sent;
    uint64_t messages_recv;
};

/* ========== API 接口 ========== */

/**
 * 初始化双内核系统
 * @param is_primary 是否为主内核
 * @param secondary_image 副内核镜像路径
 * @return 0 成功，-1 失败
 */
int dual_kernel_init(int is_primary, const char *secondary_image);

/**
 * 加载副内核镜像
 * @param path 镜像路径
 * @return 0 成功，-1 失败
 */
int dual_kernel_load(const char *path);

/**
 * 启动副内核
 * @return 0 成功，-1 失败
 */
int dual_kernel_start_secondary(void);

/**
 * 发送消息到另一个内核
 * @param type 消息类型
 * @param data 消息数据
 * @param count 数据项数量
 * @return 0 成功，-1 失败
 */
int dual_kernel_send_msg(uint64_t type, const uint64_t *data, size_t count);

/**
 * 从另一个内核接收消息
 * @param msg 消息缓冲区
 * @return 0 成功，-1 失败
 */
int dual_kernel_recv_msg(struct inter_kernel_msg *msg);

/**
 * 获取双内核管理器实例
 * @return 管理器指针
 */
struct dual_kernel_manager *dual_kernel_get_manager(void);

/**
 * VM-Exit 处理程序入口
 */
void vm_exit_handler(void);

#endif /* DUAL_KERNEL_H */
