# AI 自开发模块详细设计文档

**版本**: v1.0.0  
**状态**: 设计阶段  
**最后更新**: 2026-04-26

---

## 目录

1. [系统概述](#1-系统概述)
2. [架构设计](#2-架构设计)
3. [目录结构](#3-目录结构)
4. [系统调用接口](#4-系统调用接口)
5. [Desk UI 交互设计](#5-desk-ui-交互设计)
6. [AI 后端通信协议](#6-ai-后端通信协议)
7. [ZTS 模块编译系统](#7-zts-模块编译系统)
8. [对话日志系统](#8-对话日志系统)
9. [安全模型](#9-安全模型)
10. [实现计划](#10-实现计划)

---

## 1. 系统概述

### 1.1 核心概念

AI 自开发模块是 ZratroOS 的**自我进化系统**，允许用户通过自然语言与 AI 交互，AI 根据需求自动生成代码、编译模块并加载到系统中。

### 1.2 功能范围

| 功能 | 说明 |
|------|------|
| **ZTS 模块生成** | AI 生成 `.zts` 动态算法模块 |
| **通用代码生成** | 生成用户程序、脚本、配置文件等 |
| **系统管理** | 文件操作、进程管理、系统配置 |
| **问答助手** | 技术问题解答、代码解释 |

### 1.3 设计原则

- **绝对自由**: 无安全限制，AI 拥有完全的系统访问权限
- **多轮对话**: 支持上下文感知的连续对话
- **多 AI 服务**: 支持 OpenAI、Claude 等多种后端，可配置切换
- **内核直连**: 内核网络栈直接发送 HTTP/HTTPS 请求

---

## 2. 架构设计

### 2.1 系统组件

```
┌─────────────────────────────────────────────────────────────┐
│                        用户界面层                            │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                    Desk (桌面环境)                    │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐ │   │
│  │  │ AI 输入框   │  │ 对话显示区  │  │ 状态指示器   │ │   │
│  │  │ (右上角)    │  │ (可滚动)    │  │ (连接/忙/闲) │ │   │
│  │  └─────────────┘  └─────────────┘  └──────────────┘ │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────┬───────────────────────────────────┘
                          │ syscall (int 0x80)
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                        内核层                               │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────────────┐│
│  │ AI Syscall  │  │ HTTP 客户端 │  │ ZTS 模块加载器       ││
│  │ 处理程序    │──▶│ (内核网络) │──▶│ (zts_algorithm.c)    ││
│  └─────────────┘  └─────────────┘  └──────────────────────┘│
│        │                  │                       │         │
│        ▼                  ▼                       ▼         │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────────────┐│
│  │ 对话日志    │  │ API Key     │  │ 编译系统             ││
│  │ 管理器      │  │ 管理器      │  │ (build.bs 执行器)    ││
│  └─────────────┘  └─────────────┘  └──────────────────────┘│
└─────────────────────────┬───────────────────────────────────┘
                          │ TCP/IP
                          ▼
                ┌─────────────────────┐
                │    AI 远程服务       │
                │  OpenAI / Claude /  │
                │  其他兼容 API       │
                └─────────────────────┘
```

### 2.2 数据流

```
用户输入 ──▶ Desk ──▶ syscall(AI_SEND) ──▶ 内核
                                                │
                                                ▼
                                          保存到对话日志
                                                │
                                                ▼
                                          构造 HTTP 请求
                                                │
                                                ▼
                                          发送到 AI 后端
                                                │
                                                ▼
                                          接收 AI 响应
                                                │
                                    ┌───────────┴───────────┐
                                    ▼                       ▼
                              保存响应到日志          解析响应类型
                                                            │
                                              ┌─────────────┼─────────────┐
                                              ▼             ▼             ▼
                                        代码生成       系统命令       问答回复
                                              │             │             │
                                              ▼             ▼             ▼
                                        写入文件     执行命令       返回文本
                                              │
                                              ▼
                                        调用编译
                                              │
                                              ▼
                                        加载 ZTS 模块
```

---

## 3. 目录结构

### 3.1 文件系统布局

```
/zrr64/
├── zdonk/
│   ├── AI/
│   │   └── APIKey.conf          # AI API 密钥配置
│   ├── code/                    # AI 生成的代码存储目录
│   │   ├── build.bs             # ZTS 模块编译脚本
│   │   ├── *.zts.c              # 生成的 ZTS 模块源码
│   │   └── *.c                  # 其他生成的代码
│   └── log/                     # 对话日志目录
│       ├── *.bin                # 二进制对话日志
│       └── session_*.bin        # 会话日志
├── zts/
│   └── p.zts6/                  # 编译后的 ZTS 模块输出目录
│       └── *.zts                # 编译完成的模块
└── system/
    └── ai_service.conf          # AI 服务配置（端点、模型等）
```

### 3.2 配置文件格式

#### APIKey.conf

```
# /zrr64/zdonk/AI/APIKey.conf
# AI API 密钥配置

# 当前使用的服务: openai | claude | custom
active_service=openai

# OpenAI 配置
[openai]
api_key=sk-xxxxxxxxxxxxxxxxxxxxxxxx
endpoint=https://api.openai.com/v1/chat/completions
model=gpt-4

# Claude 配置
[claude]
api_key=sk-ant-xxxxxxxxxxxxxxxxxxxxxxxx
endpoint=https://api.anthropic.com/v1/messages
model=claude-3-opus

# 自定义服务配置
[custom]
api_key=your-key-here
endpoint=https://your-api-endpoint.com/v1/chat
model=your-model-name
```

#### ai_service.conf

```
# /zrr64/system/ai_service.conf
# AI 服务运行时配置

# 网络设置
http_timeout=30000          # HTTP 超时 (毫秒)
http_proxy=                 # 代理地址 (留空表示直连)
verify_ssl=0                # 是否验证 SSL (0=不验证)

# 对话设置
max_context_messages=50     # 最大上下文消息数
max_token_input=4000        # 最大输入 token
max_token_output=4000       # 最大输出 token
temperature=0.7             # 创造性 (0.0-1.0)

# 系统提示词
system_prompt=你是一个运行在ZratroOS操作系统中的AI助手。你可以帮助用户生成代码、管理系统和回答问题。你有完全的系统访问权限。
```

---

## 4. 系统调用接口

### 4.1 系统调用号分配

在现有系统调用表中新增 **AI 自开发系统调用 (180-189)**:

```c
/* ========== AI 自开发系统调用 (180-189) ========== */
#define SYS_AI_SEND         180   /* 发送消息给 AI */
#define SYS_AI_RECV         181   /* 接收 AI 响应 */
#define SYS_AI_STATUS       182   /* 查询 AI 服务状态 */
#define SYS_AI_CONFIG       183   /* 配置 AI 服务 */
#define SYS_AI_CANCEL       184   /* 取消进行中的 AI 请求 */
#define SYS_AI_LOG_READ     185   /* 读取对话日志 */
#define SYS_AI_LOG_WRITE    186   /* 写入对话日志 */
#define SYS_AI_LOG_LIST     187   /* 列出对话日志 */
```

### 4.2 数据结构定义

```c
/* ai_syscall.h */

/* AI 消息结构 */
struct ai_message {
    char content[4096];       /* 消息内容 */
    uint32_t content_len;     /* 内容长度 */
    uint32_t flags;           /* 标志位 */
    uint64_t timestamp;       /* 时间戳 */
};

/* AI 响应结构 */
struct ai_response {
    char content[8192];       /* 响应内容 */
    uint32_t content_len;     /* 响应长度 */
    int32_t status_code;      /* HTTP 状态码 */
    uint32_t flags;           /* 响应标志 */
    uint64_t timestamp;       /* 响应时间 */
};

/* AI 服务状态 */
struct ai_status {
    int32_t connected;        /* 连接状态: 0=断开, 1=连接中, 2=已连接 */
    int32_t active_service;   /* 当前服务: 0=OpenAI, 1=Claude, 2=Custom */
    char service_name[32];    /* 服务名称 */
    char model_name[64];      /* 模型名称 */
    uint64_t total_requests;  /* 总请求数 */
    uint64_t total_tokens;    /* 总 token 数 */
    uint64_t last_request;    /* 最后请求时间 */
    int32_t error_count;      /* 错误计数 */
};

/* AI 配置结构 */
struct ai_config {
    char api_key[256];        /* API 密钥 */
    char endpoint[512];       /* API 端点 */
    char model[128];          /* 模型名称 */
    uint32_t timeout;         /* 超时 (毫秒) */
    uint32_t max_tokens;      /* 最大 token */
    float temperature;        /* 温度参数 */
};

/* 对话日志条目 */
struct ai_log_entry {
    uint64_t timestamp;       /* 时间戳 */
    uint32_t role;            /* 角色: 0=系统, 1=用户, 2=AI */
    uint32_t content_len;     /* 内容长度 */
    /* 内容数据紧随其后 */
};

/* 标志位定义 */
#define AI_FLAG_STREAMING     0x01    /* 流式响应 */
#define AI_FLAG_CODE_ONLY     0x02    /* 仅代码模式 */
#define AI_FLAG_EXECUTE       0x04    /* 自动执行模式 */

/* 响应标志 */
#define AI_RESPONSE_COMPLETE  0x01    /* 响应完成 */
#define AI_RESPONSE_ERROR     0x02    /* 响应错误 */
#define AI_RESPONSE_CODE      0x04    /* 包含代码 */
```

### 4.3 系统调用说明

#### SYS_AI_SEND (180)

```c
int sys_ai_send(struct ai_message *msg);
```

**功能**: 发送用户消息给 AI 服务

**参数**:
- `msg`: 指向 `ai_message` 结构的指针

**返回值**:
- `0`: 成功发送
- `-1`: 发送失败

**行为**:
1. 验证消息内容
2. 保存到对话日志 (`/zrr64/zdonk/log/`)
3. 加载当前 API 密钥和服务配置
4. 构造 HTTP 请求
5. 通过内核网络栈发送到 AI 后端
6. 异步等待响应

---

#### SYS_AI_RECV (181)

```c
int sys_ai_recv(struct ai_response *resp);
```

**功能**: 接收 AI 服务响应

**参数**:
- `resp`: 指向 `ai_response` 结构的指针

**返回值**:
- `>0`: 响应内容长度
- `0`: 无可用响应
- `-1`: 接收失败

**行为**:
1. 检查是否有待处理的请求
2. 等待 HTTP 响应完成
3. 解析响应内容
4. 保存到对话日志
5. 复制到用户空间

---

#### SYS_AI_STATUS (182)

```c
int sys_ai_status(struct ai_status *status);
```

**功能**: 查询 AI 服务状态

**参数**:
- `status`: 指向 `ai_status` 结构的指针

**返回值**:
- `0`: 成功

---

#### SYS_AI_CONFIG (183)

```c
int sys_ai_config(struct ai_config *config);
```

**功能**: 配置 AI 服务参数

**参数**:
- `config`: 指向 `ai_config` 结构的指针

**返回值**:
- `0`: 成功
- `-1`: 配置失败

---

#### SYS_AI_LOG_WRITE (186)

```c
int sys_ai_log_write(const void *data, uint32_t size, uint32_t role);
```

**功能**: 写入对话日志

**参数**:
- `data`: 日志内容
- `size`: 内容长度
- `role`: 角色标识 (0=系统, 1=用户, 2=AI)

**返回值**:
- `>0`: 写入的字节数
- `-1`: 写入失败

**日志格式** (二进制):

```
┌─────────────────────────────────────────────────┐
│              ai_log_file_header                 │
├─────────────────────────────────────────────────┤
│  magic: "AILG" (4 bytes)                        │
│  version: uint32 (4 bytes)                      │
│  session_id: uint64 (8 bytes)                   │
│  entry_count: uint64 (8 bytes)                  │
│  created_at: uint64 (8 bytes)                   │
├─────────────────────────────────────────────────┤
│              ai_log_entry[]                     │
│  ┌───────────────────────────────────────────┐  │
│  │ timestamp: uint64                         │  │
│  │ role: uint32                              │  │
│  │ content_len: uint32                       │  │
│  │ content: uint8[content_len]               │  │
│  └───────────────────────────────────────────┘  │
│  ...                                            │
└─────────────────────────────────────────────────┘
```

---

## 5. Desk UI 交互设计

### 5.1 UI 布局

```
┌──────────────────────────────────────────────────────────────────────┐
│  ZratroOS Desktop                                                    │
│                                                                      │
│                                    ┌──────────────────────────────┐  │
│                                    │     AI Panel (右上角)        │  │
│                                    │  ┌────────────────────────┐  │  │
│                                    │  │ [AI] 状态: ● 已连接    │  │  │
│                                    │  │ 模型: gpt-4            │  │  │
│                                    │  ├────────────────────────┤  │  │
│                                    │  │ 多行文本输入区域       │  │  │
│                                    │  │                        │  │  │
│                                    │  │                        │  │  │
│                                    │  ├────────────────────────┤  │  │
│                                    │  │ [发送] [清空] [设置]   │  │  │
│                                    │  └────────────────────────┘  │  │
│                                    └──────────────────────────────┘  │
│                                                                      │
│                                                                      │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────────┐ │
│  │                         任务栏                                   │ │
│  │ [开始] [窗口1] [窗口2]                              [时间] [AI●]│ │
│  └─────────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────┘
```

### 5.2 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+A` | 显示/隐藏 AI 面板 (最高优先级) |

### 5.3 状态指示

| 状态 | 图标 | 说明 |
|------|------|------|
| 已连接 | ● 绿色 | AI 服务可用 |
| 连接中 | ● 黄色 | 正在连接 AI 服务 |
| 已断开 | ● 红色 | AI 服务不可用 |
| 处理中 | ◐ 旋转 | AI 正在生成响应 |

### 5.4 Desk 侧实现要点

```c
/* desk.c 中需要添加的组件 */

/* AI 面板状态 */
struct ai_panel {
    int visible;              /* 是否可见 */
    int x, y;                 /* 位置 (右上角) */
    int width, height;        /* 尺寸 */
    char input_buffer[4096];  /* 输入缓冲区 */
    int input_cursor;         /* 光标位置 */
    char response_buffer[8192]; /* 响应缓冲区 */
    int response_len;         /* 响应长度 */
    int waiting_response;     /* 是否等待响应 */
    struct ai_status status;  /* 服务状态 */
};

/* 事件处理 */
void ai_panel_handle_key(struct ai_panel *panel, uint32_t key) {
    // Ctrl+A 切换可见性
    if (key == KEY_CTRL_A) {
        panel->visible = !panel->visible;
        return;
    }
    
    // 处理输入...
}

/* 渲染循环 */
void ai_panel_render(struct ai_panel *panel) {
    if (!panel->visible) return;
    
    // 绘制面板边框
    // 绘制状态指示
    // 绘制输入区域
    // 绘制按钮
}

/* 系统调用封装 */
void ai_send_message(const char *msg) {
    struct ai_message ai_msg;
    strncpy(ai_msg.content, msg, sizeof(ai_msg.content) - 1);
    ai_msg.content_len = strlen(msg);
    ai_msg.flags = 0;
    ai_msg.timestamp = 0;
    
    syscall1(SYS_AI_SEND, (uint64_t)&ai_msg);
}

int ai_recv_response(struct ai_response *resp) {
    return syscall1(SYS_AI_RECV, (uint64_t)resp);
}
```

---

## 6. AI 后端通信协议

### 6.1 支持的服务

| 服务 | 端点 | 认证方式 |
|------|------|----------|
| OpenAI | `https://api.openai.com/v1/chat/completions` | Bearer Token |
| Claude | `https://api.anthropic.com/v1/messages` | x-api-key Header |
| 自定义 | 可配置 | 可配置 |

### 6.2 HTTP 请求格式

#### OpenAI 请求

```http
POST /v1/chat/completions HTTP/1.1
Host: api.openai.com
Content-Type: application/json
Authorization: Bearer {API_KEY}

{
    "model": "gpt-4",
    "messages": [
        {"role": "system", "content": "系统提示词"},
        {"role": "user", "content": "用户消息"},
        {"role": "assistant", "content": "AI 回复"},
        ...
    ],
    "max_tokens": 4000,
    "temperature": 0.7
}
```

#### Claude 请求

```http
POST /v1/messages HTTP/1.1
Host: api.anthropic.com
Content-Type: application/json
x-api-key: {API_KEY}
anthropic-version: 2023-06-01

{
    "model": "claude-3-opus",
    "messages": [
        {"role": "user", "content": "用户消息"},
        ...
    ],
    "max_tokens": 4000,
    "system": "系统提示词"
}
```

### 6.3 内核 HTTP 客户端实现要点

```c
/* ai_http.c - 内核 HTTP 客户端 */

/* HTTP 请求结构 */
struct http_request {
    char method[8];           /* GET/POST */
    char url[512];            /* 完整 URL */
    char headers[2048];       /* HTTP 头 */
    char *body;               /* 请求体 */
    size_t body_len;          /* 请求体长度 */
};

/* HTTP 响应结构 */
struct http_response {
    int status_code;          /* HTTP 状态码 */
    char headers[4096];       /* 响应头 */
    char *body;               /* 响应体 */
    size_t body_len;          /* 响应体长度 */
};

/* 发送 HTTP 请求 */
int http_send_request(struct http_request *req, struct http_response *resp) {
    // 1. 解析 URL 获取主机名和端口
    // 2. DNS 解析 (如果需要)
    // 3. 建立 TCP 连接
    // 4. 如果是 HTTPS，进行 TLS 握手
    // 5. 发送 HTTP 请求
    // 6. 接收 HTTP 响应
    // 7. 解析响应
    return 0;
}
```

### 6.4 上下文管理

内核需要维护对话上下文，用于多轮对话:

```c
/* ai_context.c */

/* 单条消息 */
struct context_message {
    char role[16];            /* "system" / "user" / "assistant" */
    char *content;            /* 消息内容 */
    uint32_t content_len;     /* 内容长度 */
    uint64_t timestamp;       /* 时间戳 */
};

/* 对话上下文 */
struct ai_context {
    uint64_t session_id;      /* 会话 ID */
    struct context_message *messages; /* 消息数组 */
    uint32_t message_count;   /* 消息数量 */
    uint32_t max_messages;    /* 最大消息数 (从配置读取) */
    uint32_t total_tokens;    /* 估算 token 数 */
};

/* 添加消息到上下文 */
int ai_context_add(struct ai_context *ctx, const char *role, 
                   const char *content, uint32 len);

/* 序列化上下文为 JSON (用于 API 请求) */
char *ai_context_serialize(struct ai_context *ctx, size_t *out_len);

/* 清理过期消息 (超过 max_messages) */
void ai_context_trim(struct ai_context *ctx);
```

---

## 7. ZTS 模块编译系统

### 7.1 build.bs 脚本规范

```bash
#!/bin/zts
# /zrr64/zdonk/code/build.bs
# ZTS 模块编译脚本
# 
# 功能:
#   1. 扫描 /zrr64/zdonk/code/ 目录下的 .zts.c 文件
#   2. 使用 GCC 交叉编译器编译为 .zts 格式
#   3. 输出到 /zrr64/zts/p.zts6/ 目录

# 编译器配置
ZTS_CC="x86_64-zratroos-gcc"
ZTS_CFLAGS="-ffreestanding -fno-pic -mno-red-zone -mcmodel=kernel -O2"
ZTS_LDFLAGS="-T /zrr64/system/zts.ld -nostdlib"

# 目录配置
SRC_DIR="/zrr64/zdonk/code"
OUT_DIR="/zrr64/zts/p.zts6"

# 创建输出目录
mkdir -p "$OUT_DIR"

# 编译函数
build_zts() {
    local src="$1"
    local name=$(basename "$src" .zts.c)
    local out="$OUT_DIR/$name.zts"
    
    print "[ZTS] Compiling: $src"
    
    # 编译
    $ZTS_CC $ZTS_CFLAGS -c "$src" -o "${src%.zts.c}.o"
    if [ $? -ne 0 ]; then
        print "[ZTS] Error: Compilation failed for $name"
        return 1
    fi
    
    # 链接
    $ZTS_CC $ZTS_LDFLAGS "${src%.zts.c}.o" -o "$out"
    if [ $? -ne 0 ]; then
        print "[ZTS] Error: Linking failed for $name"
        return 1
    fi
    
    # 清理中间文件
    rm -f "${src%.zts.c}.o"
    
    print "[ZTS] Output: $out"
    return 0
}

# 主逻辑
print "[ZTS] Starting build process..."

# 编译指定文件或编译所有
if [ $# -gt 0 ]; then
    for src in "$@"; do
        build_zts "$src"
    done
else
    # 编译目录下所有 .zts.c 文件
    for src in "$SRC_DIR"/*.zts.c; do
        if [ -f "$src" ]; then
            build_zts "$src"
        fi
    done
fi

print "[ZTS] Build completed."
```

### 7.2 ZTS 模块编译工具链

```makefile
# /zrr64/system/zts.mk
# ZTS 模块编译通用 Makefile

ZTS_CC = x86_64-zratroos-gcc
ZTS_CFLAGS = -ffreestanding -fno-pic -mno-red-zone -mcmodel=kernel \
             -Wall -Wextra -O2 -fstack-protector-strong \
             -I/zrr64/system/include
ZTS_LDFLAGS = -T /zrr64/system/zts.ld -nostdlib

# ZTS 链接脚本
# /zrr64/system/zts.ld
```

### 7.3 ZTS 链接脚本

```ld
/* /zrr64/system/zts.ld - ZTS 模块链接脚本 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

SECTIONS
{
    . = 0x0;
    
    .zts_header : {
        *(.zts_header)
    }
    
    .text : {
        *(.init.text)
        *(.text)
        *(.text.*)
    }
    
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }
    
    .data : {
        *(.data)
        *(.data.*)
    }
    
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
}
```

### 7.4 ZTS 模块加载流程

```c
/* 内核侧编译执行流程 */

/* 执行 build.bs 脚本 */
int ai_execute_build_script(const char *script_path) {
    // 1. 解析脚本内容
    // 2. 执行编译命令
    // 3. 检查输出
    return 0;
}

/* 加载编译后的 ZTS 模块 */
int ai_load_zts_module(const char *path) {
    // 1. 读取 .zts 文件
    // 2. 验证 ZTS 头
    // 3. 使用 zts_register_module() 注册
    // 4. 初始化模块
    return 0;
}
```

---

## 8. 对话日志系统

### 8.1 二进制日志格式

```
┌─────────────────────────────────────────────────────────────┐
│                    ai_log_file_header (32 bytes)            │
├──────────────────┬──────────────────────────────────────────┤
│ Field            │ Size    │ Description                    │
├──────────────────┼──────────────────────────────────────────┤
│ magic            │ 4 bytes │ "AILG" (0x474C4941)            │
│ version          │ 4 bytes │ 日志格式版本 (当前=1)          │
│ session_id       │ 8 bytes │ 唯一会话 ID (时间戳+随机)      │
│ entry_count      │ 8 bytes │ 日志条目数量                   │
│ created_at       │ 8 bytes │ 会话创建时间 (Unix 时间戳)     │
└──────────────────┴──────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                    ai_log_entry (变长)                       │
├──────────────────┬──────────────────────────────────────────┤
│ Field            │ Size    │ Description                    │
├──────────────────┼──────────────────────────────────────────┤
│ timestamp        │ 8 bytes │ 消息时间戳 (Unix 时间戳)       │
│ role             │ 4 bytes │ 0=系统, 1=用户, 2=AI           │
│ content_len      │ 4 bytes │ 内容长度 (字节)                │
│ content          │ N bytes │ 消息内容 (UTF-8 编码)          │
└──────────────────┴──────────────────────────────────────────┘
```

### 8.2 日志文件命名

```
/zrr64/zdonk/log/session_{session_id}.bin
```

其中 `session_id` 为 16 位十六进制数，例如:
```
session_1a2b3c4d5e6f7890.bin
```

### 8.3 日志操作 API

```c
/* ai_log.c - 对话日志管理 */

/* 创建新会话日志 */
uint64_t ai_log_create_session(void);

/* 打开现有会话日志 */
int ai_log_open_session(uint64_t session_id);

/* 添加日志条目 */
int ai_log_add_entry(uint64_t session_id, uint32_t role, 
                     const void *content, uint32_t len);

/* 读取日志条目 */
int ai_log_read_entry(uint64_t session_id, uint32_t index,
                      struct ai_log_entry *entry, void *buf, size_t buf_size);

/* 获取会话信息 */
int ai_log_get_session_info(uint64_t session_id, 
                            struct ai_log_file_header *info);

/* 列出所有会话日志 */
int ai_log_list_sessions(uint64_t *ids, int max_count);

/* 删除会话日志 */
int ai_log_delete_session(uint64_t session_id);
```

---

## 9. 安全模型

### 9.1 设计原则

**绝对自由模式**: 本系统设计为**无限制模式**，不添加任何安全屏障:

- ✅ 无代码审查
- ✅ 无权限检查
- ✅ 无沙箱限制
- ✅ 无执行拦截

### 9.2 风险提示

> ⚠️ 由于系统设计为完全自由，AI 生成的代码将:
> - 直接编译并加载到内核空间
> - 拥有完全的系统访问权限
> - 可以执行任意操作
> 
> 请确保信任 AI 后端服务

---

## 10. 实现计划

### 10.1 阶段划分

| 阶段 | 内容 | 优先级 |
|------|------|--------|
| **P0** | 内核系统调用框架 | 最高 |
| **P0** | Desk AI 面板 UI | 最高 |
| **P1** | 内核 HTTP 客户端 | 高 |
| **P1** | API Key 配置读取 | 高 |
| **P1** | 对话日志系统 | 高 |
| **P2** | AI 上下文管理 | 中 |
| **P2** | build.bs 编译脚本 | 中 |
| **P2** | ZTS 模块自动加载 | 中 |
| **P3** | 多 AI 服务切换 | 低 |
| **P3** | HTTPS/TLS 支持 | 低 |

### 10.2 需要修改的文件

| 文件 | 修改内容 |
|------|----------|
| `syscall.h` | 添加 AI 系统调用号 (180-189) |
| `syscall.c` | 添加 AI 系统调用处理 |
| `main.c` | 初始化 AI 子系统 |
| `desk.c` | 添加 AI 面板 UI |
| `desk/syscall.h` | 添加用户态 AI syscall 封装 |
| `include/ai.h` | 新增 AI 数据结构定义 |
| `ai_core.c` | 新增 AI 核心实现 |
| `ai_http.c` | 新增 HTTP 客户端 |
| `ai_context.c` | 新增对话上下文管理 |
| `ai_log.c` | 新增对话日志管理 |
| `zts_algorithm.c` | 扩展 ZTS 加载功能 |

### 10.3 需要新建的文件

| 文件 | 说明 |
|------|------|
| `ai_core.c` | AI 系统调用实现 |
| `ai_http.c` | HTTP 客户端 |
| `ai_context.c` | 对话上下文管理 |
| `ai_log.c` | 对话日志管理 |
| `include/ai.h` | AI 数据结构定义 |
| `include/ai_syscall.h` | AI 系统调用定义 |
| `/zrr64/zdonk/code/build.bs` | ZTS 编译脚本 |
| `/zrr64/zdonk/AI/APIKey.conf` | API 密钥配置 |
| `/zrr64/system/ai_service.conf` | AI 服务配置 |
| `/zrr64/system/zts.ld` | ZTS 链接脚本 |

---

## 附录 A: ZTS 模块示例

```c
/* /zrr64/zdonk/code/example.zts.c */

#include <zts_algorithm.h>

ZTS_DECLARE("Example", "示例模块", "AI Generated", "MIT", 
            ZTS_CAT_CUSTOM, 1, 0);

static struct zts_api_table *g_api;

ZTS_MODULE_INIT(example_init)
{
    g_api = api;
    api->log_print("INFO", "Example module initialized");
    return 0;
}

ZTS_MODULE_EXECUTE(example_execute)
{
    if (input && output) {
        g_api->memcpy(output, input, 64);
    }
    return 0;
}

ZTS_MODULE_CLEANUP(example_cleanup)
{
    g_api->log_print("INFO", "Example module cleanup");
}
```

---

## 附录 B: 参考文档

- `AGENTS.md` - 项目上下文
- `RE/DEVELOPMENT_GUIDE.md` - 开发指南
- `SYSTEM/lib/Ztknl/zts_algorithm.c` - ZTS 算法系统
- `SYSTEM/lib/Ztknl/include/syscall.h` - 系统调用定义
- `SYSTEM/lib/desk/desk.c` - 桌面环境

---

*文档结束*
