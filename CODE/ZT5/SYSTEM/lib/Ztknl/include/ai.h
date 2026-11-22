/*
 * Zratro OS - AI Self-Development Subsystem
 *
 * AI自开发子系统头文件
 * 提供 AI 接口给 desk，支持多轮对话、代码生成、编译加载
 *
 * @file ai.h
 * @brief AI 子系统核心数据结构与 API
 */

#ifndef AI_H
#define AI_H

#include <stdint.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * AI 子系统常量
 * ═══════════════════════════════════════════════════════════════════════════ */

#define AI_MAGIC            0x41495A00  /* "ZIA\0" */
#define AI_VERSION          1
#define AI_MAX_MESSAGE      4096        /* 最大消息长度 */
#define AI_MAX_RESPONSE     8192        /* 最大响应长度 */
#define AI_MAX_CONTEXT      50          /* 最大上下文消息数 */
#define AI_MAX_SERVICES     4           /* 最大 AI 服务数 */
#define AI_KEY_MAX          256         /* API Key 最大长度 */
#define AI_ENDPOINT_MAX     512         /* 端点 URL 最大长度 */
#define AI_MODEL_MAX        128         /* 模型名最大长度 */
#define AI_LOG_DIR          "/zrr64/zdonk/log/"
#define AI_CODE_DIR         "/zrr64/zdonk/code/"
#define AI_CONFIG_DIR       "/zrr64/zdonk/AI/"
#define AI_ZTS_OUTPUT_DIR   "/zrr64/zts/p.zts6/"
#define AI_BUILD_SCRIPT     "/zrr64/zdonk/code/build.bs"

/* ═══════════════════════════════════════════════════════════════════════════
 * 枚举定义
 * ═══════════════════════════════════════════════════════════════════════════ */

/* AI 服务类型 */
enum ai_service_type {
    AI_SERVICE_OPENAI = 0,
    AI_SERVICE_CLAUDE = 1,
    AI_SERVICE_CUSTOM = 2,
    AI_SERVICE_NONE   = 3,
};

/* 消息角色 */
enum ai_role {
    AI_ROLE_SYSTEM = 0,
    AI_ROLE_USER   = 1,
    AI_ROLE_ASSISTANT = 2,
};

/* AI 连接状态 */
enum ai_conn_state {
    AI_STATE_DISCONNECTED = 0,
    AI_STATE_CONNECTING   = 1,
    AI_STATE_CONNECTED    = 2,
    AI_STATE_ERROR        = 3,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 数据结构
 * ═══════════════════════════════════════════════════════════════════════════ */

/* AI 消息 */
struct ai_message {
    char     content[AI_MAX_MESSAGE];
    uint32_t content_len;
    uint32_t flags;
    uint64_t timestamp;
};

/* AI 响应 */
struct ai_response {
    char     content[AI_MAX_RESPONSE];
    uint32_t content_len;
    int32_t  status_code;
    uint32_t flags;
    uint64_t timestamp;
};

/* AI 服务状态 */
struct ai_status {
    int32_t  connected;
    int32_t  active_service;
    char     service_name[32];
    char     model_name[64];
    uint64_t total_requests;
    uint64_t total_tokens;
    uint64_t last_request;
    int32_t  error_count;
};

/* AI 服务配置 */
struct ai_service_config {
    int32_t  active_service;        /* 当前活跃的服务索引 */
    char     api_key[AI_KEY_MAX];
    char     endpoint[AI_ENDPOINT_MAX];
    char     model[AI_MODEL_MAX];
    uint32_t timeout_ms;
    uint32_t max_tokens;
    float    temperature;
};

/* 对话上下文单条消息 */
struct ai_ctx_message {
    char            role[16];
    char           *content;
    uint32_t        content_len;
    uint64_t        timestamp;
};

/* 对话上下文 */
struct ai_context {
    uint64_t              session_id;
    struct ai_ctx_message messages[AI_MAX_CONTEXT];
    uint32_t              message_count;
    uint32_t              total_tokens;
    uint64_t              created_at;
};

/* 二进制日志文件头 */
struct ai_log_file_header {
    uint32_t magic;         /* "AILG" = 0x474C4941 */
    uint32_t version;
    uint64_t session_id;
    uint64_t entry_count;
    uint64_t created_at;
} __attribute__((packed));

/* 二进制日志条目 */
struct ai_log_entry_header {
    uint64_t timestamp;
    uint32_t role;
    uint32_t content_len;
} __attribute__((packed));

/* 标志位 */
#define AI_FLAG_STREAMING   0x01
#define AI_FLAG_CODE_ONLY   0x02
#define AI_FLAG_EXECUTE     0x04

/* 响应标志 */
#define AI_RESPONSE_COMPLETE 0x01
#define AI_RESPONSE_ERROR    0x02
#define AI_RESPONSE_CODE     0x04

/* ═══════════════════════════════════════════════════════════════════════════
 * 内核 API
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 初始化 AI 子系统 */
int ai_subsys_init(void);

/* 发送消息给 AI (异步) */
int ai_send_message(struct ai_message *msg);

/* 接收 AI 响应 (非阻塞) */
int ai_recv_response(struct ai_response *resp);

/* 获取 AI 状态 */
int ai_get_status(struct ai_status *status);

/* 配置 AI 服务 */
int ai_set_config(struct ai_service_config *cfg);

/* 取消进行中的请求 */
int ai_cancel_request(void);

/* 对话上下文管理 */
int ai_context_init(struct ai_context *ctx);
int ai_context_add(struct ai_context *ctx, enum ai_role role,
                   const char *content, uint32_t len);
int ai_context_trim(struct ai_context *ctx);
void ai_context_free(struct ai_context *ctx);

/* 对话日志管理 */
uint64_t ai_log_create_session(void);
int ai_log_add_entry(uint64_t session_id, enum ai_role role,
                     const void *content, uint32_t len);
int ai_log_list_sessions(uint64_t *ids, int max_count);

#endif /* AI_H */
