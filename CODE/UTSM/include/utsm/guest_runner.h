#ifndef UTSM_GUEST_RUNNER_H
#define UTSM_GUEST_RUNNER_H

#include <utsm/types.h>

/* AP guest 运行器：Linux/Windows guest 的装载与 Linux 兼容层代理。
 *
 * 总线服务：
 *   kern.guest.start    → guest_runner_start()（非阻塞）
 *   kern.guest.progress → guest_progress_get()（见 guest_progress.h）
 *
 * 约定：AP 启动后 BSP 不再执行任何 VMX 指令；所有 guest 操作经
 * guest_runner_call() 邮箱转给 AP。 */

enum {
    GUEST_OP_IS_AVAILABLE = 1,
    GUEST_OP_EXEC,
    GUEST_OP_EXEC_ASYNC,
    GUEST_OP_FILE_LIST,
    GUEST_OP_FILE_READ,
    GUEST_OP_FILE_WRITE,
    GUEST_OP_FILE_PULL,
    GUEST_OP_FILE_PUSH,
    GUEST_OP_STATUS,
    GUEST_OP_RUN_SLICE
};

/* 启动 AP 运行器。返回 1 = AP 已接管 guest；0 = 旧同步路径（FUCK 关闭）。 */
int guest_runner_start(void);

/* 1 = 运行器已启动（AP 或 legacy 标记）。 */
int guest_runner_active(void);

/* 当前 CPU 是否有权直接操作 guest（AP 或 legacy BSP）。 */
int guest_runner_on_owner_cpu(void);

/* BSP → AP 邮箱代理调用。wait=0 时若 AP 忙则立即返回 0（用于 run_slice）。 */
i64 guest_runner_call_ex(u32 op, const u64 *args, u32 nargs, int wait);
i64 guest_runner_call(u32 op, const u64 *args, u32 nargs);

/* linux_compat.c 提供：AP 侧执行一个邮箱 op。 */
i64 linux_compat_ap_dispatch(u32 op, const u64 *args, u32 nargs);

#endif
