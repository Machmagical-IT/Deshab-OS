/*
 * Zratro OS - AI Self-Development Subsystem Core
 *
 * AI自开发子系统核心实现
 *
 * @file ai_core.c
 */

#include "ai.h"
#include "console.h"
#include "heap.h"
#include "string.h"
#include "vfs.h"
#include "task.h"

/* Forward declarations */
extern int ai_http_send(const char *endpoint, const char *api_key,
                        const char *model, const char *body, size_t body_len,
                        char *resp_buf, size_t resp_buf_size, int *status_code);

/* ═══════════════════════════════════════════════════════════════════════════
 * 全局状态
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct {
    int                     initialized;
    enum ai_service_type    active_service;
    struct ai_service_config services[AI_MAX_SERVICES];
    struct ai_context       context;
    uint64_t                current_session;
    int                     request_in_progress;
    struct ai_response      pending_response;
    struct ai_status        status;
} ai_global;

/* ═══════════════════════════════════════════════════════════════════════════
 * 配置加载
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 简单配置文件解析: 查找 [section] 下的 key=value */
static int ai_parse_config_value(const char *buf, size_t buf_len,
                                  const char *section, const char *key,
                                  char *out, size_t out_size) {
    const char *sec_start = NULL;
    char sec_buf[64];
    int sec_len = 0;

    /* 查找 section */
    sec_len = __builtin_snprintf(sec_buf, sizeof(sec_buf), "[%s]", section);
    for (size_t i = 0; i + sec_len <= buf_len; i++) {
        if (buf[i] == '[') {
            size_t j;
            for (j = 0; i + j < buf_len && buf[i + j] != ']' && j < sizeof(sec_buf) - 1; j++)
                ;
            if (j >= (size_t)sec_len && __builtin_memcmp(buf + i, sec_buf, sec_len - 1) == 0) {
                /* 找到 section，找下一行开始 */
                sec_start = buf + i;
                while (sec_start < buf + buf_len && *sec_start != '\n')
                    sec_start++;
                if (sec_start < buf + buf_len)
                    sec_start++;
                break;
            }
        }
    }

    if (!sec_start)
        return -1;

    /* 在 section 内查找 key= */
    char key_eq[128];
    int ke_len = __builtin_snprintf(key_eq, sizeof(key_eq), "%s=", key);

    const char *end = buf + buf_len;
    const char *p = sec_start;
    while (p + ke_len < end) {
        if (*p == '[')
            break; /* 下一个 section */
        if (__builtin_memcmp(p, key_eq, ke_len) == 0) {
            const char *val_start = p + ke_len;
            const char *val_end = val_start;
            while (val_end < end && *val_end != '\n' && *val_end != '\r')
                val_end++;
            size_t vlen = (size_t)(val_end - val_start);
            if (vlen >= out_size)
                vlen = out_size - 1;
            __builtin_memcpy(out, val_start, vlen);
            out[vlen] = '\0';
            return 0;
        }
        p++;
    }
    return -1;
}

/* 加载 API Key 配置 */
static int ai_load_api_config(void) {
    const char *config_path = "/zrr64/zdonk/AI/APIKey.conf";

    /* 尝试打开配置文件 */
    struct vfs_file *f = vfs_open(config_path, VFS_MODE_READ);
    if (f == NULL) {
        printk("ai: Config file not found: %s", config_path);
        return -1;
    }

    char buf[4096];
    long n = vfs_read(f, buf, sizeof(buf) - 1);
    vfs_close(f);

    if (n <= 0) {
        printk("ai: Failed to read config file");
        return -1;
    }
    buf[n] = '\0';

    char tmp[256];

    /* 读取 active_service */
    if (ai_parse_config_value(buf, n, "", "active_service", tmp, sizeof(tmp)) == 0) {
        if (strcmp(tmp, "openai") == 0)
            ai_global.active_service = AI_SERVICE_OPENAI;
        else if (strcmp(tmp, "claude") == 0)
            ai_global.active_service = AI_SERVICE_CLAUDE;
        else if (strcmp(tmp, "custom") == 0)
            ai_global.active_service = AI_SERVICE_CUSTOM;
    }

    /* 读取各服务配置 */
    const char *sections[] = { "openai", "claude", "custom" };
    for (int s = 0; s < 3; s++) {
        struct ai_service_config *cfg = &ai_global.services[s];

        ai_parse_config_value(buf, n, sections[s], "api_key", cfg->api_key, sizeof(cfg->api_key));
        ai_parse_config_value(buf, n, sections[s], "endpoint", cfg->endpoint, sizeof(cfg->endpoint));
        ai_parse_config_value(buf, n, sections[s], "model", cfg->model, sizeof(cfg->model));

        cfg->timeout_ms = 30000;
        cfg->max_tokens = 4000;
        cfg->temperature = 0.7f;
    }

    printk("ai: Config loaded, active service: %d", ai_global.active_service);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 公共 API 实现
 * ═══════════════════════════════════════════════════════════════════════════ */

int ai_subsys_init(void) {
    printk("=== AI Self-Development Subsystem ===");

    memset(&ai_global, 0, sizeof(ai_global));

    /* 初始化状态 */
    ai_global.status.connected = AI_STATE_DISCONNECTED;
    ai_global.status.active_service = -1;

    /* 初始化对话上下文 */
    memset(&ai_global.context, 0, sizeof(ai_global.context));
    ai_global.context.session_id = 0;

    /* 加载配置 */
    ai_load_api_config();

    /* 更新状态 */
    const char *svc_name = "unknown";
    switch (ai_global.active_service) {
    case AI_SERVICE_OPENAI:   svc_name = "OpenAI"; break;
    case AI_SERVICE_CLAUDE:   svc_name = "Claude"; break;
    case AI_SERVICE_CUSTOM:   svc_name = "Custom"; break;
    default: break;
    }

    strncpy(ai_global.status.service_name, svc_name,
            sizeof(ai_global.status.service_name) - 1);

    struct ai_service_config *cfg = &ai_global.services[ai_global.active_service];
    strncpy(ai_global.status.model_name, cfg->model,
            sizeof(ai_global.status.model_name) - 1);

    ai_global.initialized = 1;
    printk("ai: Subsystem initialized, service: %s model: %s",
           ai_global.status.service_name, ai_global.status.model_name);
    return 0;
}

int ai_send_message(struct ai_message *msg) {
    if (!ai_global.initialized || !msg || !msg->content)
        return -1;

    if (ai_global.request_in_progress)
        return -1; /* 已有请求在进行 */

    /* 将用户消息添加到上下文 */
    ai_context_add(&ai_global.context, AI_ROLE_USER,
                   msg->content, msg->content_len);

    /* 写入日志 */
    ai_log_add_entry(ai_global.current_session, AI_ROLE_USER,
                     msg->content, msg->content_len);

    /* 标记请求进行中 */
    ai_global.request_in_progress = 1;
    ai_global.status.connected = AI_STATE_CONNECTING;

    /* 序列化上下文为 JSON body */
    char body[16384];
    int body_len = 0;
    struct ai_service_config *cfg = &ai_global.services[ai_global.active_service];

    /* 构造 JSON 请求体 */
    if (ai_global.active_service == AI_SERVICE_OPENAI) {
        body_len = __builtin_snprintf(body, sizeof(body),
            "{\"model\":\"%s\",\"messages\":[", cfg->model);

        /* 添加系统提示词 */
        body_len += __builtin_snprintf(body + body_len, sizeof(body) - body_len,
            "{\"role\":\"system\",\"content\":\"You are an AI assistant running in ZratroOS. "
            "You can help generate code, manage systems, and answer questions. "
            "You have full system access. Respond in the language the user uses.\"},");

        /* 添加上下文消息 */
        for (uint32_t i = 0; i < ai_global.context.message_count; i++) {
            struct ai_ctx_message *m = &ai_global.context.messages[i];
            const char *role_str = "user";
            if (strcmp(m->role, "system") == 0) role_str = "system";
            else if (strcmp(m->role, "assistant") == 0) role_str = "assistant";

            body_len += __builtin_snprintf(body + body_len, sizeof(body) - body_len,
                "{\"role\":\"%s\",\"content\":\"", role_str);

            /* 转义 JSON 字符串 */
            for (uint32_t j = 0; j < m->content_len && body_len < (int)sizeof(body) - 10; j++) {
                char c = m->content[j];
                if (c == '\"') { body[body_len++] = '\\'; body[body_len++] = '\"'; }
                else if (c == '\\') { body[body_len++] = '\\'; body[body_len++] = '\\'; }
                else if (c == '\n') { body[body_len++] = '\\'; body[body_len++] = 'n'; }
                else { body[body_len++] = c; }
            }

            body_len += __builtin_snprintf(body + body_len, sizeof(body) - body_len, "\"},");
        }

        /* 关闭 JSON */
        body_len += __builtin_snprintf(body + body_len, sizeof(body) - body_len,
            "],\"max_tokens\":%d,\"temperature\":%.1f}", cfg->max_tokens, cfg->temperature);
    } else if (ai_global.active_service == AI_SERVICE_CLAUDE) {
        body_len = __builtin_snprintf(body, sizeof(body),
            "{\"model\":\"%s\",\"max_tokens\":%d,\"system\":\"You are an AI assistant in ZratroOS.\",\"messages\":[",
            cfg->model, cfg->max_tokens);

        for (uint32_t i = 0; i < ai_global.context.message_count; i++) {
            struct ai_ctx_message *m = &ai_global.context.messages[i];
            const char *role_str = (strcmp(m->role, "assistant") == 0) ? "assistant" : "user";

            body_len += __builtin_snprintf(body + body_len, sizeof(body) - body_len,
                "{\"role\":\"%s\",\"content\":\"", role_str);

            for (uint32_t j = 0; j < m->content_len && body_len < (int)sizeof(body) - 10; j++) {
                char c = m->content[j];
                if (c == '\"') { body[body_len++] = '\\'; body[body_len++] = '\"'; }
                else if (c == '\\') { body[body_len++] = '\\'; body[body_len++] = '\\'; }
                else if (c == '\n') { body[body_len++] = '\\'; body[body_len++] = 'n'; }
                else { body[body_len++] = c; }
            }

            body_len += __builtin_snprintf(body + body_len, sizeof(body) - body_len, "\"},");
        }

        body_len += __builtin_snprintf(body + body_len, sizeof(body) - body_len, "]}");
    }

    /* 发送 HTTP 请求 */
    char resp_buf[32768];
    int status_code = 0;

    printk("ai: Sending request to %s", cfg->endpoint);

    int ret = ai_http_send(cfg->endpoint, cfg->api_key, cfg->model,
                           body, body_len, resp_buf, sizeof(resp_buf), &status_code);

    if (ret != 0 || status_code < 200 || status_code >= 300) {
        printk("ai: HTTP request failed, status=%d", status_code);
        ai_global.request_in_progress = 0;
        ai_global.status.connected = AI_STATE_ERROR;
        ai_global.status.error_count++;
        return -1;
    }

    /* 解析 JSON 响应，提取 content */
    /* 简单查找 "content":" 后的内容 */
    const char *ct_start = NULL;
    const char *markers[] = { "\"content\":\"", "\"content\": \"" };
    for (int m = 0; m < 2 && !ct_start; m++) {
        ct_start = strstr(resp_buf, markers[m]);
        if (ct_start) ct_start += strlen(markers[m]);
    }

    if (!ct_start) {
        printk("ai: Failed to parse response content");
        ai_global.request_in_progress = 0;
        ai_global.status.connected = AI_STATE_ERROR;
        return -1;
    }

    /* 复制到响应缓冲区 */
    memset(&ai_global.pending_response, 0, sizeof(ai_global.pending_response));
    ai_global.pending_response.status_code = status_code;
    ai_global.pending_response.flags = AI_RESPONSE_COMPLETE;

    /* 提取 content 直到下一个 " 或 \" */
    const char *p = ct_start;
    while (*p && ai_global.pending_response.content_len < AI_MAX_RESPONSE - 1) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': ai_global.pending_response.content[ai_global.pending_response.content_len++] = '\n'; p++; continue;
            case 't': ai_global.pending_response.content[ai_global.pending_response.content_len++] = '\t'; p++; continue;
            case '"': ai_global.pending_response.content[ai_global.pending_response.content_len++] = '"'; p++; continue;
            case '\\': ai_global.pending_response.content[ai_global.pending_response.content_len++] = '\\'; p++; continue;
            default: break;
            }
        }
        if (*p == '"') break;
        ai_global.pending_response.content[ai_global.pending_response.content_len++] = *p++;
    }

    /* 将 AI 响应添加到上下文 */
    ai_context_add(&ai_global.context, AI_ROLE_ASSISTANT,
                   ai_global.pending_response.content,
                   ai_global.pending_response.content_len);

    /* 写入日志 */
    ai_log_add_entry(ai_global.current_session, AI_ROLE_ASSISTANT,
                     ai_global.pending_response.content,
                     ai_global.pending_response.content_len);

    ai_global.request_in_progress = 0;
    ai_global.status.connected = AI_STATE_CONNECTED;
    ai_global.status.total_requests++;

    printk("ai: Response received, len=%d", ai_global.pending_response.content_len);
    return ai_global.pending_response.content_len;
}

int ai_recv_response(struct ai_response *resp) {
    if (!resp)
        return -1;

    if (ai_global.request_in_progress)
        return 0; /* 仍在处理中 */

    if (ai_global.pending_response.content_len == 0)
        return 0; /* 无可用响应 */

    memcpy(resp, &ai_global.pending_response, sizeof(*resp));

    /* 清除待处理响应 */
    memset(&ai_global.pending_response, 0, sizeof(ai_global.pending_response));
    return resp->content_len;
}

int ai_get_status(struct ai_status *status) {
    if (!status)
        return -1;

    memcpy(status, &ai_global.status, sizeof(*status));
    return 0;
}

int ai_set_config(struct ai_service_config *cfg) {
    if (!cfg)
        return -1;

    if (cfg->active_service >= 0 && cfg->active_service < AI_MAX_SERVICES) {
        /* 这个函数签名需要调整，这里简单设置当前服务配置 */
    }

    return 0;
}

int ai_cancel_request(void) {
    if (!ai_global.request_in_progress)
        return -1;

    ai_global.request_in_progress = 0;
    ai_global.status.connected = AI_STATE_DISCONNECTED;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 对话上下文实现
 * ═══════════════════════════════════════════════════════════════════════════ */

int ai_context_init(struct ai_context *ctx) {
    if (!ctx)
        return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->session_id = ai_log_create_session();
    ai_global.current_session = ctx->session_id;
    return 0;
}

int ai_context_add(struct ai_context *ctx, enum ai_role role,
                   const char *content, uint32_t len) {
    if (!ctx || !content || len == 0)
        return -1;

    /* 如果上下文满了，删除最旧的消息 */
    if (ctx->message_count >= AI_MAX_CONTEXT) {
        /* 释放最旧消息的 content */
        if (ctx->messages[0].content) {
            kfree(ctx->messages[0].content);
        }
        /* 前移所有消息 */
        for (uint32_t i = 1; i < ctx->message_count; i++) {
            ctx->messages[i - 1] = ctx->messages[i];
        }
        ctx->message_count--;
    }

    /* 添加新消息 */
    struct ai_ctx_message *msg = &ctx->messages[ctx->message_count];

    switch (role) {
    case AI_ROLE_SYSTEM:    strncpy(msg->role, "system", sizeof(msg->role) - 1); break;
    case AI_ROLE_USER:      strncpy(msg->role, "user", sizeof(msg->role) - 1); break;
    case AI_ROLE_ASSISTANT: strncpy(msg->role, "assistant", sizeof(msg->role) - 1); break;
    }

    msg->content = (char *)kzalloc(len + 1);
    if (!msg->content)
        return -1;

    memcpy(msg->content, content, len);
    msg->content[len] = '\0';
    msg->content_len = len;
    msg->timestamp = 0; /* TODO: 时间戳 */

    ctx->message_count++;
    ctx->total_tokens += len / 4; /* 粗略估算 token 数 */
    return 0;
}

int ai_context_trim(struct ai_context *ctx) {
    if (!ctx)
        return -1;

    while (ctx->message_count > AI_MAX_CONTEXT) {
        if (ctx->messages[0].content)
            kfree(ctx->messages[0].content);

        for (uint32_t i = 1; i < ctx->message_count; i++)
            ctx->messages[i - 1] = ctx->messages[i];

        ctx->message_count--;
    }
    return 0;
}

void ai_context_free(struct ai_context *ctx) {
    if (!ctx)
        return;

    for (uint32_t i = 0; i < ctx->message_count; i++) {
        if (ctx->messages[i].content)
            kfree(ctx->messages[i].content);
    }
    memset(ctx, 0, sizeof(*ctx));
}
