/*
 * Zratro OS - PEXLF Protocol Implementation
 * 
 * PE-ELF Cross-Format Link Protocol
 * 允许 ELF 和 PE 程序互相通信的统一 IPC 协议
 * 
 * @file pexlf.c
 * @brief PEXLF 协议核心实现
 * 
 * 功能特性:
 *   - 统一的二进制格式无关消息传递
 *   - 基于命名通道的发布/订阅模式
 *   - 支持单播、广播、组播
 *   - 自动检测发送者格式 (ELF/PE)
 *   - 完整的错误处理和边界检查
 * 
 * @author ZratroOS Team
 * @version 1.0
 * @date 2026
 */

#include "pexlf.h"
#include "task.h"
#include "ipc.h"
#include "heap.h"
#include "string.h"
#include "console.h"
#include "vmm.h"
#include "ntdll.h"

/* ========== 编译时断言 (确保结构体大小正确) ========== */

_Static_assert(sizeof(struct pexlf_header) == 128, 
               "pexlf_header size must be 128 bytes");

/* ========== 全局状态 ========== */

/** 通道表最大容量 */
#define PEXLF_MAX_CHANNELS      256

/** 端点表最大容量 */
#define PEXLF_MAX_ENDPOINTS     512

/** 消息队列最大长度 */
#define PEXLF_MSG_QUEUE_SIZE    1024

/** 默认缓冲区大小 (字节) */
#define PEXLF_BUFFER_SIZE       4096

/** 消息保留时间 (毫秒) */
#define PEXLF_MSG_TTL_MS        5000

/** 通道名称最大长度 */
#define PEXLF_CHANNEL_NAME_MAX  32

_Static_assert(PEXLF_MAX_CHANNELS <= 256, 
               "PEXLF_MAX_CHANNELS cannot exceed 256");
_Static_assert(PEXLF_MAX_ENDPOINTS <= 512, 
               "PEXLF_MAX_ENDPOINTS cannot exceed 512");

/* 全局通道表 - 存储所有活动通道 */
static struct pexlf_channel channels[PEXLF_MAX_CHANNELS];

/** @brief 当前活动的通道数量 */
static int channel_count = 0;

/* 全局端点表 - 跟踪所有连接的进程 */
static struct pexlf_endpoint endpoints[PEXLF_MAX_ENDPOINTS];

/** @brief 当前活动的端点数量 */
static int endpoint_count = 0;

/** @brief PEXLF 子系统全局锁 (保护共享资源) */
static spinlock_t pexlf_lock = SPINLOCK_INIT;

/* ========== 内部数据结构 ========== */

/**
 * @brief 内部消息结构 (包含用户数据和元数据)
 */
struct pexlf_message {
    struct pexlf_header header;     /**< 消息头部 (136 字节) */
    void *payload;                  /**< 消息负载数据指针 */
    size_t payload_size;            /**< 负载数据大小 (字节) */
    uint64_t enqueue_time;          /**< 入队时间戳 (tick) */
    uint64_t ttl;                   /**< 生存时间 (毫秒) */
    struct pexlf_message *next;     /**< 下一条消息 (链表) */
};

/**
 * @brief 消息队列结构 (FIFO)
 */
struct pexlf_queue {
    struct pexlf_message *head;     /**< 队列头 (出队端) */
    struct pexlf_message *tail;     /**< 队列尾 (入队端) */
    int count;                      /**< 当前消息数量 */
    spinlock_t lock;                /**< 队列锁 (SMP 安全) */
};

/** @brief 全局消息队列实例 */
static struct pexlf_queue global_queue = {
    .head = NULL,
    .tail = NULL,
    .count = 0,
    .lock = SPINLOCK_INIT,
};

/* ========== 辅助函数 ========== */

/**
 * @brief 计算数据的校验和 (用于消息完整性验证)
 * 
 * @param data 数据指针
 * @param size 数据大小 (字节)
 * @return uint64_t 校验和值
 * 
 * @note 使用改进的 XOR-SUM 算法，可检测单比特和多比特错误
 */
static uint64_t pexlf_checksum(const void *data, size_t size) {
    if (data == NULL || size == 0) {
        return 0;
    }
    
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t sum = 0;
    uint64_t xor_acc = 0;
    
    /* 第一遍：累加和 */
    for (size_t i = 0; i < size; i++) {
        sum += bytes[i];
    }
    
    /* 第二遍：异或累积 */
    for (size_t i = 0; i < size; i += 8) {
        uint64_t word = 0;
        size_t remaining = size - i;
        
        if (remaining >= 8) {
            memcpy(&word, &bytes[i], 8);
        } else {
            memcpy(&word, &bytes[i], remaining);
        }
        
        xor_acc ^= word;
    }
    
    return (sum << 32) | (xor_acc & 0xFFFFFFFF);
}

/**
 * @brief 验证 PEXLF 消息头的完整性和有效性 (保留供未来使用)
 * 
 * @param hdr 消息头指针
 * @return int 0=有效，-1=无效
 * 
 * @note 目前未使用，保留用于未来的严格验证模式
 * 
 * @note 检查项目:
 *   - Magic number 匹配
 *   - 主版本号兼容
 *   - 头部大小正确
 *   - 校验和匹配
 */
__attribute__((unused))
static int pexlf_validate_header(struct pexlf_header *hdr) {
    if (hdr == NULL) {
        return -PEXLF_ERR_INVALID;
    }
    
    /* 1. 检查 Magic Number */
    if (hdr->magic != PEXLF_MAGIC) {
        println("[PEXLF] Error: Invalid magic number");
        return -PEXLF_ERR_FORMAT;
    }
    
    /* 2. 检查主版本号 */
    uint32_t major = (hdr->version >> 16) & 0xFF;
    
    if (major != PEXLF_VERSION_MAJOR) {
        println("[PEXLF] Error: Version mismatch");
        return -PEXLF_ERR_VERSION;
    }
    
    /* 3. 检查头部大小 */
    if (hdr->header_size != sizeof(struct pexlf_header)) {
        println("[PEXLF] Error: Invalid header size");
        return -PEXLF_ERR_FORMAT;
    }
    
    /* 4. 验证校验和 */
    uint64_t calc_checksum = pexlf_checksum(hdr, sizeof(struct pexlf_header) - 8);
    if (calc_checksum != hdr->checksum) {
        println("[PEXLF] Error: Checksum mismatch");
        return -PEXLF_ERR_FORMAT;
    }
    
    /* 5. 检查消息类型有效性 */
    if (hdr->msg_type < PEXLF_MSG_HANDSHAKE || 
        hdr->msg_type > PEXLF_MSG_CLOSE) {
        println("[PEXLF] Error: Invalid message type");
        return -PEXLF_ERR_FORMAT;
    }
    
    /* 6. 检查负载大小合理性 */
    if (hdr->payload_size > (1024 * 1024)) {  /* 最大 1MB */
        println("[PEXLF] Error: Payload too large");
        return -PEXLF_ERR_OVERFLOW;
    }
    
    return 0;
}

/**
 * @brief 在通道表中查找指定名称的通道
 * 
 * @param name 通道名称 (以 null 结尾的字符串)
 * @return struct pexlf_channel* 找到的通道指针，未找到返回 NULL
 * 
 * @note 此函数不获取锁，调用者需确保线程安全
 */
static struct pexlf_channel *pexlf_find_channel(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    
    for (int i = 0; i < channel_count; i++) {
        /* 快速检查：首字符不匹配则跳过 */
        if (channels[i].name[0] != name[0]) {
            continue;
        }
        
        if (strcmp(channels[i].name, name) == 0) {
            return &channels[i];
        }
    }
    
    return NULL;
}

/**
 * @brief 为当前进程创建或获取 PEXLF 端点
 * 
 * @return struct pexlf_endpoint* 端点指针，失败返回 NULL
 * 
 * @note 每个进程只有一个端点，重复调用返回同一端点
 */
static struct pexlf_endpoint *pexlf_create_endpoint(void) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return NULL;
    }
    
    spin_lock(&pexlf_lock);
    
    /* 检查是否已存在该进程的端点 */
    for (int i = 0; i < endpoint_count; i++) {
        if (endpoints[i].pid == proc->pid && endpoints[i].state != 0) {
            spin_unlock(&pexlf_lock);
            return &endpoints[i];
        }
    }
    
    /* 检查端点表是否已满 */
    if (endpoint_count >= PEXLF_MAX_ENDPOINTS) {
        println("[PEXLF] Error: Endpoint table full");
        spin_unlock(&pexlf_lock);
        return NULL;
    }
    
    /* 分配新端点 */
    struct pexlf_endpoint *ep = &endpoints[endpoint_count++];
    memset(ep, 0, sizeof(struct pexlf_endpoint));  /* 清零初始化 */
    
    ep->endpoint_id = (uint64_t)(uintptr_t)ep;
    ep->pid = proc->pid;
    /* TODO: 获取当前 TID */
    ep->tid = 0;
    /* 简化：默认都为 ELF 程序，PE 程序通过特殊 API 注册 */
    ep->format = 0;  /* 0=ELF, 1=PE */
    ep->state = 1;   /* ACTIVE */
    ep->channel_count = 0;
    
    spin_unlock(&pexlf_lock);
    
    return ep;
}

/* ========== 核心 API 实现 ========== */

/**
 * @brief 初始化 PEXLF 协议子系统
 * 
 * @return int 0=成功，-1=失败
 * 
 * @note 此函数在内核启动时由 init.c 调用一次
 *       初始化通道表、端点表和消息队列
 */
int pexlf_init(void) {
    if (channel_count != 0 || endpoint_count != 0) {
        println("[PEXLF] Warning: Already initialized");
        return -1;
    }
    
    println("[PEXLF] Initializing protocol subsystem...");
    
    /* 初始化通道表 - 清零所有条目 */
    memset(channels, 0, sizeof(channels));
    channel_count = 0;
    
    /* 初始化端点表 - 清零所有条目 */
    memset(endpoints, 0, sizeof(endpoints));
    endpoint_count = 0;
    
    /* 初始化全局消息队列 */
    memset(&global_queue, 0, sizeof(global_queue));
    global_queue.lock = SPINLOCK_INIT;
    
    println("[PEXLF] Protocol initialized successfully");
    return 0;
}

/**
 * @brief 创建或查找命名通道
 * 
 * @param name 通道名称 (唯一标识符)
 * @return uint64_t 通道 ID，失败返回 (uint64_t)-1
 * 
 * @note 如果通道已存在则增加引用计数
 *       通道名长度不能超过 PEXLF_MAX_CHANNEL_NAME-1
 */
uint64_t pexlf_channel_create(const char *name) {
    if (!name || name[0] == '\0') {
        println("[PEXLF] Error: NULL or empty channel name");
        return (uint64_t)-PEXLF_ERR_INVALID;
    }
    
    size_t name_len = strlen(name);
    if (name_len >= PEXLF_MAX_CHANNEL_NAME) {
        println("[PEXLF] Error: Channel name too long");
        return (uint64_t)-PEXLF_ERR_OVERFLOW;
    }
    
    spin_lock(&pexlf_lock);
    
    /* 检查通道是否已存在 */
    struct pexlf_channel *existing = pexlf_find_channel(name);
    if (existing) {
        existing->ref_count++;
        println("[PEXLF] Channel already exists (incrementing ref count)");
        spin_unlock(&pexlf_lock);
        return existing->channel_id;
    }
    
    /* 检查通道表容量 */
    if (channel_count >= PEXLF_MAX_CHANNELS) {
        println("[PEXLF] Error: Channel table full");
        spin_unlock(&pexlf_lock);
        return (uint64_t)-PEXLF_ERR_NO_MEMORY;
    }
    
    /* 分配新通道 */
    struct pexlf_channel *ch = &channels[channel_count++];
    memset(ch, 0, sizeof(struct pexlf_channel));
    strncpy(ch->name, name, PEXLF_MAX_CHANNEL_NAME - 1);
    ch->channel_id = (uint64_t)(uintptr_t)ch;  /* 使用地址作为 ID */
    ch->ref_count = 1;
    ch->flags = 0;
    ch->created_time = 0;  /* TODO: 实现时间戳 */
    ch->last_activity = 0;
    ch->total_messages = 0;
    
    println("[PEXLF] Channel created successfully");
    
    spin_unlock(&pexlf_lock);
    return ch->channel_id;
}

/**
 * @brief 加入现有通道或自动创建
 * 
 * @param name 通道名称
 * @return int 0=成功，-1=失败
 * 
 * @note 如果通道不存在则自动创建
 *       增加通道的引用计数
 */
int pexlf_channel_join(const char *name) {
    if (!name || name[0] == '\0') {
        return -PEXLF_ERR_INVALID;
    }
    
    struct pexlf_channel *ch = pexlf_find_channel(name);
    if (!ch) {
        /* 自动创建通道 */
        uint64_t id = pexlf_channel_create(name);
        return (id == (uint64_t)-1) ? -1 : 0;
    }
    
    spin_lock(&pexlf_lock);
    
    /* 增加引用计数 */
    ch->ref_count++;
    println("[PEXLF] Channel joined");
    
    spin_unlock(&pexlf_lock);
    return 0;
}

/**
 * @brief 离开指定通道 (减少引用计数)
 * 
 * @param channel_id 通道 ID
 * @return int 0=成功，-1=未找到通道
 * 
 * @note 当引用计数降为 0 时删除通道
 *       使用数组压缩算法保持通道表紧凑
 */
int pexlf_channel_leave(uint64_t channel_id) {
    if (channel_id == 0) {
        return -PEXLF_ERR_INVALID;
    }
    
    spin_lock(&pexlf_lock);
    
    for (int i = 0; i < channel_count; i++) {
        if (channels[i].channel_id == channel_id) {
            channels[i].ref_count--;
            println("[PEXLF] Channel leave (decrementing ref count)");
            
            /* 引用计数为 0 时删除通道 */
            if (channels[i].ref_count <= 0) {
                println("[PEXLF] Channel removed (ref count reached 0)");
                
                /* 从数组中移除该条目 */
                channels[i].name[0] = '\0';
                channel_count--;
                
                /* 如果要删除的不是最后一个元素，则移动最后一个元素到当前位置 */
                if (i < channel_count) {
                    memmove(&channels[i], &channels[channel_count], 
                            sizeof(struct pexlf_channel));
                }
                
                /* 清零被移动的最后一个元素 */
                memset(&channels[channel_count], 0, sizeof(struct pexlf_channel));
            }
            
            spin_unlock(&pexlf_lock);
            return 0;
        }
    }
    
    spin_unlock(&pexlf_lock);
    println("[PEXLF] Error: Channel not found");
    return -PEXLF_ERR_NO_CHANNEL;
}

/**
 * @brief 向指定通道发送消息
 * 
 * @param channel 目标通道名称
 * @param type 消息类型 (PEXLF_MSG_*)
 * @param data 数据指针
 * @param size 数据大小 (字节)
 * @return int PEXLF_ERR_OK=成功，其他=错误码
 * 
 * @note 自动注册发送者端点
 *       消息被添加到全局队列等待接收者
 */
int pexlf_send(const char *channel, pexlf_msg_type_t type, 
               const void *data, size_t size) {
    /* 参数验证 */
    if (!channel || !data || size == 0) {
        println("[PEXLF] Error: Invalid send parameters");
        return PEXLF_ERR_INVALID;
    }
    
    if (strlen(channel) >= PEXLF_MAX_CHANNEL_NAME) {
        println("[PEXLF] Error: Channel name too long");
        return PEXLF_ERR_OVERFLOW;
    }
    
    /* 确保发送者已注册 */
    struct pexlf_endpoint *ep = pexlf_create_endpoint();
    if (!ep) {
        println("[PEXLF] Error: Failed to create endpoint");
        return PEXLF_ERR_NO_MEMORY;
    }
    
    /* 分配消息结构 */
    struct pexlf_message *msg = kmalloc(sizeof(struct pexlf_message));
    if (!msg) {
        println("[PEXLF] Error: Failed to allocate message");
        return PEXLF_ERR_NO_MEMORY;
    }
    
    /* 填充消息头 */
    memset(&msg->header, 0, sizeof(struct pexlf_header));
    msg->header.magic = PEXLF_MAGIC;
    msg->header.version = (PEXLF_VERSION_MAJOR << 16) | PEXLF_VERSION_MINOR;
    msg->header.header_size = sizeof(struct pexlf_header);
    msg->header.msg_type = type;
    msg->header.data_type = PEXLF_TYPE_BYTES;  /* TODO: 支持其他类型 */
    msg->header.msg_id = (uint64_t)(uintptr_t)msg;
    msg->header.sender_pid = ep->pid;
    msg->header.sender_tid = ep->tid;
    msg->header.sender_format = ep->format;
    msg->header.payload_size = size;
    msg->header.flags = 0;
    msg->header.priority = 4;  /* 默认优先级：普通 */
    
    strncpy(msg->header.channel, channel, 31);
    
    /* 计算校验和 (不包含 checksum 字段本身) */
    msg->header.checksum = pexlf_checksum(&msg->header, 
                                          sizeof(struct pexlf_header) - 8);
    
    /* 分配并复制负载数据 */
    msg->payload = kmalloc(size);
    if (!msg->payload) {
        kfree(msg);
        println("[PEXLF] Error: Failed to allocate payload");
        return PEXLF_ERR_NO_MEMORY;
    }
    memcpy(msg->payload, data, size);
    msg->payload_size = size;
    
    /* 初始化消息元数据 */
    msg->enqueue_time = 0;  /* TODO: 获取时间戳 */
    msg->ttl = PEXLF_MSG_TTL_MS;
    msg->next = NULL;
    
    /* 加入全局队列 */
    spin_lock(&global_queue.lock);
    
    msg->next = NULL;
    if (global_queue.tail) {
        global_queue.tail->next = msg;
    } else {
        global_queue.head = msg;
    }
    global_queue.tail = msg;
    global_queue.count++;
    
    println("[PEXLF] Message sent successfully");
    
    /* 更新通道统计信息 */
    struct pexlf_channel *ch = pexlf_find_channel(channel);
    if (ch) {
        ch->total_messages++;
        ch->last_activity = 0;  /* TODO: 更新时间戳 */
    }
    
    spin_unlock(&global_queue.lock);
    
    return PEXLF_ERR_OK;
}

/**
 * @brief 从指定通道接收消息 (阻塞式)
 * 
 * @param channel 要监听的通道名称
 * @param buf 用户提供的缓冲区指针
 * @param buf_size 缓冲区大小 (字节)
 * @param timeout 超时时间 (毫秒)，0 表示立即返回
 * @return int 成功返回读取的字节数，失败返回错误码
 * 
 * @note 查找队列中匹配通道的消息并移除
 *       数据会被复制到用户缓冲区
 */
int pexlf_recv(const char *channel, void *buf, size_t buf_size, 
               uint64_t timeout) {
    (void)timeout;  /* TODO: 实现超时机制 */
    
    /* 参数验证 */
    if (!channel || !buf || buf_size == 0) {
        println("[PEXLF] Error: Invalid receive parameters");
        return -PEXLF_ERR_INVALID;
    }
    
    if (strlen(channel) >= PEXLF_MAX_CHANNEL_NAME) {
        println("[PEXLF] Error: Channel name too long");
        return -PEXLF_ERR_OVERFLOW;
    }
    
    struct pexlf_message *msg = NULL;
    
    /* 从队列中查找并移除匹配通道的消息 */
    spin_lock(&global_queue.lock);
    
    struct pexlf_message **prev = &global_queue.head;
    struct pexlf_message *curr = global_queue.head;
    
    while (curr) {
        if (strcmp(curr->header.channel, channel) == 0) {
            /* 找到匹配的消息 */
            *prev = curr->next;
            if (!curr->next) {
                /* 如果删除的是尾节点，更新 tail */
                global_queue.tail = (*prev == NULL && global_queue.head == NULL) ? NULL : *prev;
            }
            global_queue.count--;
            msg = curr;
            println("[PEXLF] Message received");
            break;
        }
        prev = &curr->next;
        curr = curr->next;
    }
    
    spin_unlock(&global_queue.lock);
    
    if (!msg) {
        println("[PEXLF] Error: No message available for channel");
        return -PEXLF_ERR_TIMEOUT;  /* 无可用消息 */
    }
    
    /* 复制数据到用户缓冲区，防止溢出 */
    size_t copy_size = (msg->payload_size < buf_size) ? 
                       msg->payload_size : buf_size;
    memcpy(buf, msg->payload, copy_size);
    
    /* 清理消息资源 */
    kfree(msg->payload);
    kfree(msg);
    
    return (int)copy_size;
}

/**
 * @brief 非阻塞式接收消息 (立即返回)
 * 
 * @param channel 通道名称
 * @param buf 缓冲区指针
 * @param buf_size 缓冲区大小
 * @return int 成功返回读取字节数，失败返回错误码
 */
int pexlf_try_recv(const char *channel, void *buf, size_t buf_size) {
    return pexlf_recv(channel, buf, buf_size, 0);
}

/**
 * @brief 向所有通道广播消息
 * 
 * @param type 消息类型
 * @param data 数据指针
 * @param size 数据大小
 * @return int PEXLF_ERR_OK=成功
 * 
 * @note 遍历所有活动通道并发送消息副本
 *       用于系统通知等场景
 */
int pexlf_broadcast(pexlf_msg_type_t type, const void *data, size_t size) {
    if (!data || size == 0) {
        println("[PEXLF] Error: Invalid broadcast parameters");
        return -PEXLF_ERR_INVALID;
    }
    
    spin_lock(&pexlf_lock);
    
    if (channel_count == 0) {
        println("[PEXLF] Error: No active channels for broadcast");
        spin_unlock(&pexlf_lock);
        return -PEXLF_ERR_NO_CHANNEL;
    }
    
    println("[PEXLF] Broadcasting to all channels...");
    
    /* 向每个通道发送消息 */
    for (int i = 0; i < channel_count; i++) {
        if (channels[i].name[0] != '\0') {
            pexlf_send(channels[i].name, type, data, size);
        }
    }
    
    spin_unlock(&pexlf_lock);
    return PEXLF_ERR_OK;
}

/**
 * @brief 列出所有活动通道的名称
 * 
 * @param names 二维数组用于存储通道名 [max_count][PEXLF_MAX_CHANNEL_NAME]
 * @param max_count 最大返回数量
 * @return int 实际返回的通道数量
 */
int pexlf_list_channels(char names[][PEXLF_MAX_CHANNEL_NAME], int max_count) {
    if (!names || max_count <= 0) {
        println("[PEXLF] Error: Invalid list_channels parameters");
        return -PEXLF_ERR_INVALID;
    }
    
    spin_lock(&pexlf_lock);
    
    /* 限制返回数量为实际通道数和请求数量的较小值 */
    int count = (channel_count < max_count) ? channel_count : max_count;
    
    for (int i = 0; i < count; i++) {
        strncpy(names[i], channels[i].name, PEXLF_MAX_CHANNEL_NAME - 1);
        names[i][PEXLF_MAX_CHANNEL_NAME - 1] = '\0';  /* 确保 null 终止 */
    }
    
    println("[PEXLF] Listed channels");
    
    spin_unlock(&pexlf_lock);
    return count;
}

/**
 * @brief 获取指定进程的对等方信息
 * 
 * @param pid 进程 ID
 * @param info 输出缓冲区指针
 * @return int 0=成功，PEXLF_ERR_NO_CHANNEL=未找到
 */
int pexlf_get_peer_info(uint64_t pid, struct pexlf_endpoint *info) {
    if (!info) {
        println("[PEXLF] Error: NULL info pointer");
        return -PEXLF_ERR_INVALID;
    }
    
    spin_lock(&pexlf_lock);
    
    for (int i = 0; i < endpoint_count; i++) {
        if (endpoints[i].pid == pid && endpoints[i].state != 0) {
            memcpy(info, &endpoints[i], sizeof(struct pexlf_endpoint));
            println("[PEXLF] Found peer information");
            spin_unlock(&pexlf_lock);
            return 0;
        }
    }
    
    spin_unlock(&pexlf_lock);
    println("[PEXLF] Error: Peer not found");
    return PEXLF_ERR_NO_CHANNEL;
}

/**
 * @brief 清理 PEXLF 子系统所有资源
 * 
 * @note 释放所有消息、通道和端点
 *       通常在系统关闭时调用
 */
void pexlf_cleanup(void) {
    println("[PEXLF] Cleaning up subsystem...\n");
    
    spin_lock(&pexlf_lock);
    
    /* 统计要清理的资源数量 */
    int msg_count = 0;
    struct pexlf_message *msg = global_queue.head;
    while (msg) {
        msg_count++;
        msg = msg->next;
    }
    
    /* 清理所有消息 */
    msg = global_queue.head;
    while (msg) {
        struct pexlf_message *next = msg->next;
        kfree(msg->payload);
        kfree(msg);
        msg = next;
    }
    global_queue.head = NULL;
    global_queue.tail = NULL;
    global_queue.count = 0;
    
    /* 清零通道表 */
    memset(channels, 0, sizeof(channels));
    channel_count = 0;
    
    /* 清零端点表 */
    memset(endpoints, 0, sizeof(endpoints));
    endpoint_count = 0;
    
    println("[PEXLF] Cleanup completed successfully");
    
    spin_unlock(&pexlf_lock);
}
