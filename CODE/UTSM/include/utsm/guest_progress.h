#ifndef UTSM_GUEST_PROGRESS_H
#define UTSM_GUEST_PROGRESS_H

#include <utsm/types.h>

/* Guest 装载进度：AP 运行器写，BSP 桌面读（kern.guest.progress）。
 * 共享结构只放 32 位枚举/百分比，中文文案由消费方映射，避免跨 CPU 字符串撕裂。 */
#define GUEST_PROGRESS_MAGIC 0x47505247u   /* 'GPRG' */
#define GUEST_PROGRESS_PCT_UNKNOWN 0xFFFFFFFFu

enum {
    GUEST_SLOT_LINUX = 0,
    GUEST_SLOT_WINDOWS = 1,
    GUEST_SLOT_COUNT = 2
};

enum {
    GUEST_PHASE_OFF = 0,
    GUEST_PHASE_PREP,      /* 装载镜像/构造页表 */
    GUEST_PHASE_LAUNCH,    /* vmlaunch */
    GUEST_PHASE_KERNEL,    /* guest 内核启动 */
    GUEST_PHASE_DAEMON,    /* guest 用户态/daemon 就绪 */
    GUEST_PHASE_POST,      /* lib 同步 / VSCode 安装 */
    GUEST_PHASE_READY,
    GUEST_PHASE_FAILED
};

typedef struct {
    u32 magic;
    u32 enabled;      /* FUCK 是否请求该 guest */
    u32 phase;        /* GUEST_PHASE_* */
    u32 percent;      /* 0..100；UNKNOWN = 不确定 */
    u32 detail;       /* 可选子步骤编号 */
    u32 updated_ms;   /* 运行器 uptime 低 32 位 */
} utsm_guest_progress;

typedef struct {
    u32 magic;
    u32 abi_version;
    u32 count;
    utsm_guest_progress slot[GUEST_SLOT_COUNT];
} utsm_guest_progress_table;

const utsm_guest_progress_table *guest_progress_get(void);
void guest_progress_set(u32 slot, u32 enabled, u32 phase, u32 percent, u32 detail);
void guest_progress_note_serial(char c);

#endif
