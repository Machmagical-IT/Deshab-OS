#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* 系统调用号 */
#define SYS_EXIT        0
#define SYS_READ        1
#define SYS_WRITE       2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_MMAP        11
#define SYS_MUNMAP      12
#define SYS_GETFBINFO   156
#define SYS_FB_SYNC     155

/* AI 自开发系统调用 (180-189) */
#define SYS_AI_SEND         180
#define SYS_AI_RECV         181
#define SYS_AI_STATUS       182
#define SYS_AI_CONFIG       183
#define SYS_AI_CANCEL       184
#define SYS_AI_LOG_READ     185
#define SYS_AI_LOG_WRITE    186
#define SYS_AI_LOG_LIST     187

/* 输入子系统 */
#define SYS_INPUT_POLL      188

/* AI 数据结构 (用户空间简化版) */
struct ai_message {
    char     content[4096];
    uint32_t content_len;
    uint32_t flags;
    uint64_t timestamp;
};

struct ai_response {
    char     content[8192];
    uint32_t content_len;
    int32_t  status_code;
    uint32_t flags;
    uint64_t timestamp;
};

struct ai_status_info {
    int32_t  connected;
    int32_t  active_service;
    char     service_name[32];
    char     model_name[64];
    uint64_t total_requests;
    uint64_t total_tokens;
    uint64_t last_request;
    int32_t  error_count;
};

/* 基本系统调用包装 */
void sys_exit(int status);
ssize_t sys_write(int fd, const void *buf, size_t count);
ssize_t sys_read(int fd, void *buf, size_t count);
int sys_open(const char *path, int flags, int mode);
int sys_close(int fd);

/* AI 系统调用包装 */
static inline int ai_send(struct ai_message *msg) {
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_AI_SEND), "D"(msg)
        : "memory"
    );
    return ret;
}

static inline int ai_recv(struct ai_response *resp) {
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_AI_RECV), "D"(resp)
        : "memory"
    );
    return ret;
}

static inline int ai_get_status(struct ai_status_info *status) {
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_AI_STATUS), "D"(status)
        : "memory"
    );
    return ret;
}

static inline int ai_cancel(void) {
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_AI_CANCEL)
        : "memory"
    );
    return ret;
}

/* 输入系统调用包装 */
/* 事件类型定义 */
#define INPUT_EVENT_KEY_PRESS   1
#define INPUT_EVENT_KEY_RELEASE 2
#define INPUT_EVENT_MOUSE_MOVE_X 3
#define INPUT_EVENT_MOUSE_MOVE_Y 4
#define INPUT_EVENT_MOUSE_LEFT  5
#define INPUT_EVENT_MOUSE_RIGHT 6

static inline int input_poll(uint32_t *events, int max_events) {
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_INPUT_POLL), "D"(events), "S"(max_events)
        : "memory"
    );
    return ret;
}

#endif
