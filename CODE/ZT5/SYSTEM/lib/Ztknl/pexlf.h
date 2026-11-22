/*
 * Zratro OS - PEXLF Protocol Header
 * 
 * PE-ELF Cross-Format Link Protocol
 * 允许 ELF 和 PE 程序互相通信的统一 IPC 协议
 * 
 * @file pexlf.h
 * @brief PEXLF 协议定义和 API 声明
 * 
 * 主要特性:
 *   - 二进制格式透明 (ELF/PE 程序可互操作)
 *   - 基于命名通道的发布/订阅模式
 *   - 支持单播、广播、组播
 *   - 完整的消息类型系统
 *   - 校验和验证确保完整性
 * 
 * 使用示例:
 *   // 发送端
 *   pexlf_channel_create("my_channel");
 *   pexlf_send("my_channel", PEXLF_MSG_DATA, buf, size);
 *   
 *   // 接收端
 *   pexlf_channel_join("my_channel");
 *   pexlf_recv("my_channel", buf, sizeof(buf), 0);
 * 
 * @author ZratroOS Team
 * @version 1.0
 * @date 2026
 */

#ifndef PEXLF_H
#define PEXLF_H

#include "ktypes.h"

/* ========== 协议版本 ========== */

/** PEXLF 协议主版本号 - 不兼容的更改时递增 */
#define PEXLF_VERSION_MAJOR     1

/** PEXLF 协议次版本号 - 向后兼容的功能添加 */
#define PEXLF_VERSION_MINOR     0

/** PEXLF Magic Number - 用于标识协议消息 
 * ASCII: "PEXLF\0\0\0" = 0x5045584C46000000
 */
#define PEXLF_MAGIC             0x5045584C46000000ULL

/* ========== 消息类型 ========== */

/**
 * @brief PEXLF 消息类型枚举
 * 
 * 用于标识消息的用途和语义
 */
typedef enum {
    PEXLF_MSG_INVALID       = 0,    /**< 无效消息 (默认值) */
    PEXLF_MSG_HANDSHAKE     = 1,    /**< 握手请求 - 建立连接时使用 */
    PEXLF_MSG_HANDSHAKE_ACK = 2,    /**< 握手确认 - 响应握手请求 */
    PEXLF_MSG_DATA          = 3,    /**< 数据消息 - 传输实际数据 */
    PEXLF_MSG_COMMAND       = 4,    /**< 命令消息 - 发送控制指令 */
    PEXLF_MSG_RESPONSE      = 5,    /**< 响应消息 - 回复命令 */
    PEXLF_MSG_EVENT         = 6,    /**< 事件通知 - 异步事件通知 */
    PEXLF_MSG_ERROR         = 7,    /**< 错误消息 - 报告错误 */
    PEXLF_MSG_CLOSE         = 8,    /**< 关闭连接 - 结束通信 */
} pexlf_msg_type_t;

/* ========== 数据类型 ========== */

/**
 * @brief PEXLF 支持的数据类型
 * 
 * 用于描述消息负载的序列化格式
 */
typedef enum {
    PEXLF_TYPE_NONE         = 0,    /**< 无类型/空值 */
    PEXLF_TYPE_INT8         = 1,    /**< 8 位有符号整数 */
    PEXLF_TYPE_INT16        = 2,    /**< 16 位有符号整数 */
    PEXLF_TYPE_INT32        = 3,    /**< 32 位有符号整数 */
    PEXLF_TYPE_INT64        = 4,    /**< 64 位有符号整数 */
    PEXLF_TYPE_UINT8        = 5,    /**< 8 位无符号整数 */
    PEXLF_TYPE_UINT16       = 6,    /**< 16 位无符号整数 */
    PEXLF_TYPE_UINT32       = 7,    /**< 32 位无符号整数 */
    PEXLF_TYPE_UINT64       = 8,    /**< 64 位无符号整数 */
    PEXLF_TYPE_FLOAT        = 9,    /**< 32 位浮点数 */
    PEXLF_TYPE_DOUBLE       = 10,   /**< 64 位双精度浮点数 */
    PEXLF_TYPE_STRING       = 11,   /**< UTF-8 字符串 */
    PEXLF_TYPE_BYTES        = 12,   /**< 原始字节数组 */
    PEXLF_TYPE_HANDLE       = 13,   /**< 文件/资源句柄 */
} pexlf_data_type_t;

/* ========== 消息头 (固定 136 字节) ========== */

/**
 * @brief PEXLF 消息头部结构
 * 
 * 所有 PEXLF 消息都以此头部开头，后跟负载数据
 * 大小固定为 136 字节，确保协议一致性
 * 
 * 内存布局:
 *   0-7:   magic (8)
 *   8-11:  version (4)
 *   12-15: header_size (4)
 *   16-19: msg_type (4)
 *   20-23: data_type (4)
 *   24-31: msg_id (8)
 *   32-39: correlation_id (8)
 *   40-47: sender_pid (8)
 *   48-55: sender_tid (8)
 *   56-59: sender_format (4)
 *   60-63: reserved1 (4)
 *   64-71: timestamp (8)
 *   72-75: flags (4)
 *   76-79: priority (4)
 *   80-87: payload_size (8)
 *   88-95: checksum (8)
 *   96-127: channel[32] (32)
 *   Total: 128 bytes (without padding)
 */
struct pexlf_header {
    uint64_t magic;             /**< Magic Number: PEXLF_MAGIC */
    uint32_t version;           /**< 协议版本：(major << 16 | minor) */
    uint32_t header_size;       /**< 头部大小：固定 128 */
    
    uint32_t msg_type;          /**< 消息类型：pexlf_msg_type_t */
    uint32_t data_type;         /**< 数据类型：pexlf_data_type_t */
    
    uint64_t msg_id;            /**< 消息唯一标识符 */
    uint64_t correlation_id;    /**< 关联消息 ID：用于请求/响应配对 */
    
    uint64_t sender_pid;        /**< 发送者进程 ID */
    uint64_t sender_tid;        /**< 发送者线程 ID */
    uint32_t sender_format;     /**< 发送者格式：0=ELF, 1=PE */
    uint32_t reserved1;         /**< 保留字段 (对齐用) */
    
    uint64_t timestamp;         /**< 时间戳：系统 tick count */
    uint32_t flags;             /**< 消息标志：PEXLF_FLAG_* */
    uint32_t priority;          /**< 优先级：0-7 (0 最高，7 最低) */
    
    uint64_t payload_size;      /**< 负载数据大小 (字节) */
    uint64_t checksum;          /**< 校验和：覆盖头部 (除 checksum 本身) */
    
    char channel[32];           /**< 通道名称：null 终止字符串 */
};

/* ========== 消息标志位 ========== */

/** 分片消息标志 - 表示消息被分片 */
#define PEXLF_FLAG_FRAGMENT     0x01

/** 最后一片标志 - 表示分片的最后一片 */
#define PEXLF_FLAG_LAST_FRAG    0x02

/** 压缩标志 - 负载已压缩 */
#define PEXLF_FLAG_COMPRESSED   0x04

/** 加密标志 - 负载已加密 */
#define PEXLF_FLAG_ENCRYPTED    0x08

/** 异步标志 - 异步消息无需响应 */
#define PEXLF_FLAG_ASYNC        0x10

/** 广播标志 - 广播消息 */
#define PEXLF_FLAG_BROADCAST    0x20

/* ========== 错误码定义 ========== */

/**
 * @brief PEXLF 错误码枚举
 * 
 * 所有 PEXLF API 函数返回的错误码
 */
typedef enum {
    PEXLF_ERR_OK            = 0,    /**< 成功 */
    PEXLF_ERR_INVALID       = 1,    /**< 无效参数 */
    PEXLF_ERR_NO_MEMORY     = 2,    /**< 内存不足 */
    PEXLF_ERR_NO_CHANNEL    = 3,    /**< 通道不存在 */
    PEXLF_ERR_ACCESS        = 4,    /**< 访问权限拒绝 */
    PEXLF_ERR_TIMEOUT       = 5,    /**< 操作超时 */
    PEXLF_ERR_OVERFLOW      = 6,    /**< 缓冲区溢出 */
    PEXLF_ERR_FORMAT        = 7,    /**< 格式错误/不兼容 */
    PEXLF_ERR_VERSION       = 8,    /**< 版本不匹配 */
} pexlf_error_t;

/* ========== 通道管理 ========== */

/** 通道名称最大长度 (包含 null 终止符) */
#define PEXLF_MAX_CHANNEL_NAME  32

/** 最大通道数量限制 */
#define PEXLF_MAX_CHANNELS      256

/**
 * @brief PEXLF 通道结构
 * 
 * 表示一个命名通信通道，支持多进程订阅
 */
struct pexlf_channel {
    char name[PEXLF_MAX_CHANNEL_NAME];    /**< 通道名称 */
    uint64_t channel_id;                    /**< 通道唯一 ID */
    uint32_t ref_count;                     /**< 引用计数 (订阅者数量) */
    uint32_t flags;                         /**< 通道标志 */
    uint64_t created_time;                  /**< 创建时间戳 */
    uint64_t last_activity;                 /**< 最后活动时间 */
    uint64_t total_messages;                /**< 累计消息数量 */
};

/* ========== 连接端点 ========== */

/**
 * @brief PEXLF 端点结构
 * 
 * 每个参与通信的进程都有一个端点，记录其状态和能力
 */
struct pexlf_endpoint {
    uint64_t endpoint_id;                 /**< 端点唯一 ID */
    uint64_t pid;                         /**< 所属进程 ID */
    uint64_t tid;                         /**< 主线程 ID */
    uint32_t format;                      /**< 二进制格式：0=ELF, 1=PE */
    uint32_t state;                       /**< 端点状态：0=INACTIVE, 1=ACTIVE */
    struct pexlf_channel *channels[16];   /**< 加入的通道列表 */
    int channel_count;                    /**< 加入的通道数量 */
};

/* ========== API 函数声明 ========== */

/**
 * @brief 初始化 PEXLF 协议子系统
 * @return int 0=成功，-1=失败
 * 
 * @note 在内核启动时调用一次，初始化所有内部数据结构
 */
int pexlf_init(void);

/**
 * @brief 创建新的通信通道
 * @param name 通道名称 (唯一标识符)
 * @return uint64_t 通道 ID，失败返回错误码
 * 
 * @note 如果通道已存在则增加引用计数
 */
uint64_t pexlf_channel_create(const char *name);

/**
 * @brief 加入现有通道或自动创建
 * @param name 通道名称
 * @return int 0=成功，-1=失败
 */
int pexlf_channel_join(const char *name);

/**
 * @brief 离开指定通道
 * @param channel_id 通道 ID
 * @return int 0=成功，-1=失败
 * 
 * @note 当引用计数降为 0 时通道会被删除
 */
int pexlf_channel_leave(uint64_t channel_id);

/**
 * @brief 向指定通道发送消息
 * @param channel 目标通道名称
 * @param type 消息类型
 * @param data 数据指针
 * @param size 数据大小
 * @return int PEXLF_ERR_OK=成功，其他=错误码
 */
int pexlf_send(const char *channel, pexlf_msg_type_t type, 
               const void *data, size_t size);

/**
 * @brief 向指定进程发送单播消息
 * @param target_pid 目标进程 ID
 * @param type 消息类型
 * @param data 数据指针
 * @param size 数据大小
 * @return int PEXLF_ERR_OK=成功，其他=错误码
 * 
 * @note TODO: 实现点对点直接通信
 */
int pexlf_send_to(uint64_t target_pid, pexlf_msg_type_t type,
                  const void *data, size_t size);

/**
 * @brief 从通道接收消息 (阻塞式)
 * @param channel 通道名称
 * @param buf 缓冲区指针
 * @param buf_size 缓冲区大小
 * @param timeout 超时时间 (毫秒)，0 表示立即返回
 * @return int 成功返回读取字节数，失败返回负值
 */
int pexlf_recv(const char *channel, void *buf, size_t buf_size, 
               uint64_t timeout);

/**
 * @brief 非阻塞式接收消息
 * @param channel 通道名称
 * @param buf 缓冲区指针
 * @param buf_size 缓冲区大小
 * @return int 成功返回读取字节数，失败返回负值
 */
int pexlf_try_recv(const char *channel, void *buf, size_t buf_size);

/**
 * @brief 向所有通道广播消息
 * @param type 消息类型
 * @param data 数据指针
 * @param size 数据大小
 * @return int PEXLF_ERR_OK=成功
 */
int pexlf_broadcast(pexlf_msg_type_t type, const void *data, size_t size);

/**
 * @brief 列出所有活动通道
 * @param names 二维数组用于存储通道名
 * @param max_count 最大返回数量
 * @return int 实际返回的通道数量
 */
int pexlf_list_channels(char names[][PEXLF_MAX_CHANNEL_NAME], int max_count);

/**
 * @brief 获取指定进程的端点信息
 * @param pid 进程 ID
 * @param info 输出缓冲区指针
 * @return int 0=成功，PEXLF_ERR_NO_CHANNEL=未找到
 */
int pexlf_get_peer_info(uint64_t pid, struct pexlf_endpoint *info);

/**
 * @brief 清理 PEXLF 子系统所有资源
 * 
 * @note 释放所有消息、通道和端点
 *       通常在系统关闭时调用
 */
void pexlf_cleanup(void);

#endif /* PEXLF_H */
