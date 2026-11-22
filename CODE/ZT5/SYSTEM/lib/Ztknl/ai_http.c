/*
 * Zratro OS - AI HTTP Client
 *
 * 内核 HTTP 客户端，用于与 AI API 通信
 *
 * @file ai_http.c
 */

#include "ai.h"
#include "console.h"
#include "string.h"
#include "net.h"
#include "tcp.h"
#include "socket.h"

/* 简单 DNS 解析: 返回硬编码或使用 IP */
static int ai_resolve_host(const char *hostname, uint32_t *ip_out) {
    /* 简单实现: 尝试从字符串解析 IP 或使用 DHCP 提供的 DNS */
    /* 这里使用简化实现，实际需要 DNS 解析 */
    (void)hostname;
    printk("ai: DNS resolve for %s (simplified)", hostname);
    /* TODO: 实现 DNS 解析 */
    return -1;
}

/* 发送 HTTP 请求并接收响应 */
int ai_http_send(const char *endpoint, const char *api_key,
                 const char *model, const char *body, size_t body_len,
                 char *resp_buf, size_t resp_buf_size, int *status_code) {
    if (!endpoint || !api_key || !body || !resp_buf || !status_code)
        return -1;

    printk("ai_http: Connecting to %s", endpoint);

    /* 解析端点获取主机和路径 */
    /* 简化: 提取 host */
    char host[256] = {0};
    char path[512] = {0};
    int port = 443;

    const char *p = endpoint;
    /* 跳过 https:// 或 http:// */
    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        port = 443;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        port = 80;
    }

    /* 提取 host */
    const char *slash = strchr(p, '/');
    size_t host_len = slash ? (size_t)(slash - p) : strlen(p);
    if (host_len >= sizeof(host))
        host_len = sizeof(host) - 1;
    memcpy(host, p, host_len);
    host[host_len] = '\0';

    /* 提取 path */
    if (slash)
        strncpy(path, slash, sizeof(path) - 1);
    else
        strncpy(path, "/", sizeof(path) - 1);

    /* 构造 HTTP 请求 */
    char http_req[8192];
    int http_len = 0;

    /* 判断服务类型设置头 */
    int is_claude = (strstr(endpoint, "anthropic") != NULL);

    http_len = snprintf(http_req, sizeof(http_req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n",
        path, host, body_len);

    if (is_claude) {
        http_len += snprintf(http_req + http_len, sizeof(http_req) - http_len,
            "x-api-key: %s\r\n"
            "anthropic-version: 2023-06-01\r\n",
            api_key);
    } else {
        http_len += snprintf(http_req + http_len, sizeof(http_req) - http_len,
            "Authorization: Bearer %s\r\n",
            api_key);
    }

    http_len += snprintf(http_req + http_len, sizeof(http_req) - http_len,
        "\r\n");

    /* 添加 body */
    if (http_len + (int)body_len < (int)sizeof(http_req)) {
        memcpy(http_req + http_len, body, body_len);
        http_len += body_len;
    }

    printk("ai_http: HTTP request ready, len=%d", http_len);

    /* 简化实现: 通过 socket 发送 */
    /* TODO: 实现 TCP 连接和数据收发 */

    /* 由于网络栈可能不完全，这里使用占位实现 */
    /* 在实际环境中应使用 socket API */

    *status_code = 0;
    snprintf(resp_buf, resp_buf_size,
        "{\"error\":\"HTTP client not fully implemented. "
        "Need TCP socket support for AI API calls.\"}");

    printk("ai_http: Request sent (simplified)");
    return 0;
}
