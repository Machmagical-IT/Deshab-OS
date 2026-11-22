/*
 * Zratro OS - AI Dialog Log System
 *
 * 对话日志管理，二进制格式存储
 *
 * @file ai_log.c
 */

#include "ai.h"
#include "console.h"
#include "heap.h"
#include "string.h"
#include "vfs.h"
#include "cpu.h"

#define AI_LOG_MAGIC  0x474C4941  /* "AILG" */
#define AI_LOG_VERSION 1
#define AI_LOG_MAX_SESSIONS 256
#define AI_LOG_MAX_ENTRIES 4096

static struct {
    uint64_t sessions[AI_LOG_MAX_SESSIONS];
    uint32_t session_count;
    uint64_t next_session_id;
} ai_log_global;

/* 生成 session ID */
static uint64_t ai_generate_session_id(void) {
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    return tsc ^ (tsc >> 32);
}

uint64_t ai_log_create_session(void) {
    if (ai_log_global.session_count >= AI_LOG_MAX_SESSIONS) {
        printk("ai_log: Session table full");
        return 0;
    }

    uint64_t sid = ai_generate_session_id();
    if (sid == 0) sid = ++ai_log_global.next_session_id;

    ai_log_global.sessions[ai_log_global.session_count++] = sid;

    /* 创建日志文件 */
    char path[256];
    snprintf(path, sizeof(path), "%ssession_%lx.bin", AI_LOG_DIR, sid);

    /* 确保目录存在 */
    struct vfs_file *f = vfs_open(path, VFS_MODE_READ);
    if (f) {
        vfs_close(f);
        return sid; /* 已存在 */
    }

    /* 写入文件头 */
    struct ai_log_file_header hdr;
    hdr.magic = AI_LOG_MAGIC;
    hdr.version = AI_LOG_VERSION;
    hdr.session_id = sid;
    hdr.entry_count = 0;
    hdr.created_at = 0; /* TODO: 时间戳 */

    f = vfs_open(path, VFS_MODE_WRITE | VFS_MODE_CREATE);
    if (f) {
        vfs_write(f, &hdr, sizeof(hdr));
        vfs_close(f);
        printk("ai_log: New session created: %lx", sid);
    } else {
        printk("ai_log: Failed to create session file: %s", path);
    }

    return sid;
}

int ai_log_add_entry(uint64_t session_id, enum ai_role role,
                     const void *content, uint32_t len) {
    if (!content || len == 0)
        return -1;

    if (session_id == 0) {
        /* 使用当前 session */
        session_id = ai_log_global.session_count > 0 ?
                     ai_log_global.sessions[ai_log_global.session_count - 1] : 0;
        if (session_id == 0)
            session_id = ai_log_create_session();
    }

    if (session_id == 0)
        return -1;

    char path[256];
    snprintf(path, sizeof(path), "%ssession_%lx.bin", AI_LOG_DIR, session_id);

    /* 打开文件追加写入 */
    struct vfs_file *f = vfs_open(path, VFS_MODE_WRITE);
    if (!f) {
        /* 文件不存在则创建 */
        f = vfs_open(path, VFS_MODE_WRITE | VFS_MODE_CREATE);
        if (f) {
            struct ai_log_file_header hdr;
            hdr.magic = AI_LOG_MAGIC;
            hdr.version = AI_LOG_VERSION;
            hdr.session_id = session_id;
            hdr.entry_count = 0;
            hdr.created_at = 0;
            vfs_write(f, &hdr, sizeof(hdr));
        }
    }

    if (!f) {
        printk("ai_log: Failed to open session file");
        return -1;
    }

    /* 写入条目头 */
    struct ai_log_entry_header entry;
    entry.timestamp = 0; /* TODO: 时间戳 */
    entry.role = role;
    entry.content_len = len;

    vfs_write(f, &entry, sizeof(entry));

    /* 写入内容 */
    vfs_write(f, content, len);

    vfs_close(f);
    return 0;
}

int ai_log_list_sessions(uint64_t *ids, int max_count) {
    if (!ids || max_count <= 0)
        return -1;

    int count = (max_count < (int)ai_log_global.session_count) ?
                max_count : (int)ai_log_global.session_count;

    for (int i = 0; i < count; i++)
        ids[i] = ai_log_global.sessions[i];

    return count;
}
