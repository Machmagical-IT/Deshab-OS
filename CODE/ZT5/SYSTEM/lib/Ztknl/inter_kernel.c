/*
 * Zratro OS - Inter-Kernel Communication Implementation
 *
 * 双内核通信模块实现
 */

#include "inter_kernel.h"
#include "apic.h"
#include "pmm.h"
#include "smp.h"
#include "idt.h"
#include "console.h"
#include "string.h"

/* ========== 全局状态 ========== */

static struct inter_kernel_shm *shm = NULL;
static inter_kernel_role_t my_role = INTER_KERNEL_ROLE_NONE;
static int initialized = 0;

/* 消息 ID 计数器 */
static volatile uint64_t msg_id_counter = 0;

/* 获取下一个消息 ID */
static inline uint64_t get_next_msg_id(void) {
    return __sync_add_and_fetch(&msg_id_counter, 1);
}

/* 获取对方的 APIC ID */
static uint32_t get_peer_apic_id(void) {
    uint32_t my_apic = apic_get_id();

    if (my_role == INTER_KERNEL_ROLE_PRIMARY) {
        /* 主内核 -> 获取 Azknl 的 APIC ID */
        if (shm && shm->header.azknl_ready) {
            return shm->header.azknl_apic_id;
        }
    } else if (my_role == INTER_KERNEL_ROLE_SECONDARY) {
        /* 辅助内核 -> 获取 Ztknl 的 APIC ID */
        if (shm && shm->header.ztknl_ready) {
            return shm->header.ztknl_apic_id;
        }
    }

    /* 默认返回不同的 APIC ID（假设对方与自己不同） */
    return my_apic + 1;
}

/* ========== 初始化 ========== */

/**
 * 初始化双内核通信
 * 必须在内存管理初始化后调用
 */
/* 外部角色变量声明 */
extern int primary_role;

int inter_kernel_init(void) {
    if (initialized) {
        print("[INTER-KERNEL] Already initialized\n");
        return 0;
    }

    print("[INTER-KERNEL] Initializing...\n");

    /* 获取共享内存虚拟地址 */
    uint64_t phys_addr = INTER_KERNEL_SHM_PHYS;
    shm = (struct inter_kernel_shm *)phys_to_virt(phys_addr);

    if (shm == NULL) {
        print("[INTER-KERNEL] ERROR: phys_to_virt failed\n");
        return -1;
    }

    /* 根据 primary_role 决定角色
     * primary_role: 0=未知, 1=ztknl主, 2=azknl主
     * my_role: 1=PRIMARY(Ztknl), 2=SECONDARY(Azknl)
     */
    int my_role;
    if (primary_role == 1) {
        /* Ztknl 为主内核 */
        my_role = INTER_KERNEL_ROLE_PRIMARY;
    } else if (primary_role == 2) {
        /* Azknl 为主内核，Ztknl 为辅助 */
        my_role = INTER_KERNEL_ROLE_SECONDARY;
    } else {
        /* 未知角色，按默认逻辑 */
        my_role = (shm->header.magic != INTER_KERNEL_MAGIC) ?
                  INTER_KERNEL_ROLE_PRIMARY : INTER_KERNEL_ROLE_SECONDARY;
    }

    /* 检查是否已有数据（判断主从） */
    if (shm->header.magic != INTER_KERNEL_MAGIC) {
        /* 首次初始化，这是主内核 */
        memset((void *)shm, 0, sizeof(struct inter_kernel_shm));

        shm->header.magic = INTER_KERNEL_MAGIC;
        shm->header.version = INTER_KERNEL_VERSION;
        shm->header.shm_phys = phys_addr;
        shm->header.shm_size = INTER_KERNEL_SHM_SIZE;

        /* 初始化队列容量 */
        shm->ztknl_to_azknl.capacity = INTER_KERNEL_QUEUE_MSG_COUNT;
        shm->azknl_to_ztknl.capacity = INTER_KERNEL_QUEUE_MSG_COUNT;

        /* 根据 primary_role 决定哪个内核就绪 */
        if (my_role == INTER_KERNEL_ROLE_PRIMARY) {
            /* Ztknl 为主内核 */
            shm->header.ztknl_ready = 1;
            shm->header.ztknl_cpu_count = smp_cpu_count;
            shm->header.ztknl_apic_id = apic_get_id();
            print("[INTER-KERNEL] Initialized as PRIMARY (Ztknl)\n");
        } else {
            /* Azknl 为主内核，但 Ztknl 先初始化了共享内存 */
            shm->header.ztknl_ready = 1;
            shm->header.ztknl_cpu_count = smp_cpu_count;
            shm->header.ztknl_apic_id = apic_get_id();
            print("[INTER-KERNEL] Initialized as SECONDARY (Ztknl)\n");
        }
    } else {
        /* 已有数据，这是辅助内核 */
        if (my_role == INTER_KERNEL_ROLE_PRIMARY) {
            /* Ztknl 为主内核，但 Azknl 已经初始化了共享内存 */
            shm->header.ztknl_ready = 1;
            shm->header.ztknl_cpu_count = smp_cpu_count;
            shm->header.ztknl_apic_id = apic_get_id();
            print("[INTER-KERNEL] Initialized as PRIMARY (Ztknl)\n");
        } else {
            /* Azknl 为主内核，Ztknl 为辅助 */
            shm->header.azknl_ready = 1;
            shm->header.azknl_cpu_count = smp_cpu_count;
            shm->header.azknl_apic_id = apic_get_id();
            print("[INTER-KERNEL] Initialized as SECONDARY (Azknl)\n");
        }
    }

    /* 注册 IPI 中断处理程序 - 双向注册 */
    isr_register_handler(IPI_ZTKNL_TO_AZKNL, inter_kernel_ipi_handler);
    isr_register_handler(IPI_AZKNL_TO_ZTKNL, inter_kernel_ipi_handler);
    print("[INTER-KERNEL] IPI handlers registered\n");

    /* 等待对方就绪 */
    int timeout = 1000;  /* 1秒超时 */
    while (timeout > 0) {
        if (my_role == INTER_KERNEL_ROLE_PRIMARY && shm->header.azknl_ready) {
            print("[INTER-KERNEL] Peer (Azknl) is ready\n");
            break;
        } else if (my_role == INTER_KERNEL_ROLE_SECONDARY && shm->header.ztknl_ready) {
            print("[INTER-KERNEL] Peer (Ztknl) is ready\n");
            break;
        }
        /* 简单的延时 */
        for (volatile int i = 0; i < 100000; i++);
        timeout--;
    }

    if (timeout <= 0) {
        print("[INTER-KERNEL] WARNING: Peer not ready yet\n");
    }

    initialized = 1;

    print("[INTER-KERNEL] Init complete\n");
    print("[INTER-KERNEL] Ztknl ready\n");
    print("[INTER-KERNEL] Azknl ready\n");

    return 0;
}

/* ========== API 实现 ========== */

inter_kernel_role_t inter_kernel_get_role(void) {
    return my_role;
}

int inter_kernel_is_peer_ready(void) {
    if (!shm) return 0;

    if (my_role == INTER_KERNEL_ROLE_PRIMARY) {
        return shm->header.azknl_ready;
    } else if (my_role == INTER_KERNEL_ROLE_SECONDARY) {
        return shm->header.ztknl_ready;
    }

    return 0;
}

int inter_kernel_send_msg(uint64_t type, const uint64_t *data, size_t count) {
    if (!shm || !initialized) {
        return -1;
    }

    /* 检查对方是否就绪 */
    if (!inter_kernel_is_peer_ready()) {
        return -1;
    }

    /* 选择目标队列 */
    struct inter_kernel_queue *queue;
    if (my_role == INTER_KERNEL_ROLE_PRIMARY) {
        queue = &shm->ztknl_to_azknl;
    } else {
        queue = &shm->azknl_to_ztknl;
    }

    /* 检查队列是否满 */
    if (queue->count >= queue->capacity) {
        return -2;  /* 队列满 */
    }

    /* 获取写入位置 */
    uint32_t idx = queue->tail;

    /* 写入消息 */
    queue->messages[idx].id = get_next_msg_id();
    queue->messages[idx].type = type;
    queue->messages[idx].timestamp = 0;  /* TODO: 添加时间戳 */

    for (size_t i = 0; i < count && i < 5; i++) {
        queue->messages[idx].data[i] = data[i];
    }

    /* 内存屏障 */
    __sync_synchronize();

    /* 更新队列状态 */
    queue->tail = (idx + 1) % queue->capacity;
    __sync_add_and_fetch(&queue->count, 1);

    /* 更新头部计数器 */
    if (my_role == INTER_KERNEL_ROLE_PRIMARY) {
        __sync_add_and_fetch(&shm->header.ztknl_to_azknl_count, 1);
    } else {
        __sync_add_and_fetch(&shm->header.azknl_to_ztknl_count, 1);
    }

    /* 发送 IPI 通知对方 */
    inter_kernel_notify_peer();

    return 0;
}

int inter_kernel_recv_msg(struct inter_kernel_msg *msg) {
    if (!shm || !initialized) {
        return -1;
    }

    /* 选择源队列 */
    struct inter_kernel_queue *queue;
    if (my_role == INTER_KERNEL_ROLE_PRIMARY) {
        queue = &shm->azknl_to_ztknl;
    } else {
        queue = &shm->ztknl_to_azknl;
    }

    /* 检查队列是否空 */
    if (queue->count == 0) {
        return -2;  /* 队列空 */
    }

    /* 获取读取位置 */
    uint32_t idx = queue->head;

    /* 读取消息 */
    msg->id = queue->messages[idx].id;
    msg->type = queue->messages[idx].type;
    msg->timestamp = queue->messages[idx].timestamp;

    for (int i = 0; i < 5; i++) {
        msg->data[i] = queue->messages[idx].data[i];
    }

    /* 内存屏障 */
    __sync_synchronize();

    /* 更新队列状态 */
    queue->head = (idx + 1) % queue->capacity;
    __sync_sub_and_fetch(&queue->count, 1);

    /* 更新头部计数器 */
    if (my_role == INTER_KERNEL_ROLE_PRIMARY) {
        __sync_sub_and_fetch(&shm->header.azknl_to_ztknl_count, 1);
    } else {
        __sync_sub_and_fetch(&shm->header.ztknl_to_azknl_count, 1);
    }

    return 0;
}

uint32_t inter_kernel_pending_msgs(void) {
    if (!shm) return 0;

    if (my_role == INTER_KERNEL_ROLE_PRIMARY) {
        return shm->azknl_to_ztknl.count;
    } else {
        return shm->ztknl_to_azknl.count;
    }
}

void inter_kernel_notify_peer(void) {
    if (!shm) return;

    uint32_t peer_apic = get_peer_apic_id();
    uint8_t ipi_vector;

    if (my_role == INTER_KERNEL_ROLE_PRIMARY) {
        ipi_vector = IPI_ZTKNL_TO_AZKNL;
    } else {
        ipi_vector = IPI_AZKNL_TO_ZTKNL;
    }

    /* 设置 IPI 标志 */
    shm->header.ipi_flag = 1;
    shm->header.ipi_data = (uint64_t)my_role;

    /* 发送 IPI */
    apic_send_ipi(peer_apic, ipi_vector, 0);
}

void inter_kernel_ipi_handler(struct interrupt_frame *frame) {
    (void)frame;

    if (!shm) {
        apic_eoi();
        return;
    }

    /* 读取 IPI 数据 */
    if (shm->header.ipi_flag) {
        shm->header.ipi_flag = 0;
        print("[INTER-KERNEL] IPI received\n");
    }

    /* 处理待处理消息 */
    struct inter_kernel_msg msg;
    while (inter_kernel_recv_msg(&msg) == 0) {
        print("[INTER-KERNEL] MSG received\n");

        /* 根据消息类型处理 */
        switch (msg.type) {
            case MSG_TYPE_HELLO:
                print("[INTER-KERNEL] Received HELLO from peer\n");
                break;
            case MSG_TYPE_READY:
                print("[INTER-KERNEL] Peer is READY\n");
                break;
            case MSG_TYPE_PING:
                /* 响应 Pong */
                {
                    uint64_t pong_data = msg.data[0];
                    inter_kernel_send_msg(MSG_TYPE_PONG, &pong_data, 1);
                }
                break;
            case MSG_TYPE_PONG:
                print("[INTER-KERNEL] Received PONG\n");
                break;
            case MSG_TYPE_SHUTDOWN:
                print("[INTER-KERNEL] Peer requested SHUTDOWN\n");
                break;
            default:
                print("[INTER-KERNEL] Unknown msg type\n");
                break;
        }
    }

    /* 发送 EOI */
    apic_eoi();
}

int inter_kernel_shutdown_notify(void) {
    print("[INTER-KERNEL] Sending SHUTDOWN notification to peer\n");
    return inter_kernel_send_msg(MSG_TYPE_SHUTDOWN, NULL, 0);
}

void inter_kernel_get_shm_info(uint64_t *base, uint64_t *size) {
    if (base) *base = INTER_KERNEL_SHM_PHYS;
    if (size) *size = INTER_KERNEL_SHM_SIZE;
}