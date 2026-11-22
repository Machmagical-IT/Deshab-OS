#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "idt.h"
#include "ktypes.h"   /* 引入统一的 pid_t, ssize_t 等类型定义 */

/* 系统调用所需的结构体定义 */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct sysinfo {
    uint64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
};

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
    uint64_t tv_sec;
    uint64_t tv_usec;
};
#endif

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

/* ========== 基础系统调用 (0-39) ========== */

#define SYS_EXIT        0
#define SYS_READ        1
#define SYS_WRITE       2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_SEEK        5
#define SYS_STAT        6
#define SYS_EXEC        7
#define SYS_YIELD       8
#define SYS_SLEEP       9
#define SYS_GETPID      10
#define SYS_GETPPID     11    /* 获取父进程 PID */
#define SYS_UNAME       12    /* 获取系统信息 */
#define SYS_SYSINFO     13    /* 获取系统详细信息 */
#define SYS_GETTIMEOFDAY 14   /* 获取时间 */
#define SYS_TIME        15    /* 获取时间戳 */
#define SYS_KILL        16    /* 发送信号 */
#define SYS_SIGNAL      17    /* 注册信号处理器 */
#define SYS_GETSOCKNAME 18    /* 获取套接字名称 */
#define SYS_MMAP        19
#define SYS_MUNMAP      20
#define SYS_BRK         21
#define SYS_GETCWD      22
#define SYS_CHDIR       23
#define SYS_IOCTL       24
#define SYS_MKDIR       25
#define SYS_RMDIR       26
#define SYS_UNLINK      27
#define SYS_OPENDIR     28
#define SYS_READDIR     29
#define SYS_CLOSEDIR    30
#define SYS_FORK        31
#define SYS_WAIT        32
#define SYS_PIPE        33
#define SYS_DUP         34
#define SYS_DUP2        35
#define SYS_EXECVE      36
#define SYS_EXECVEAT    37
#define SYS_SOCKET      38
#define SYS_BIND        39

/* ========== 用户态事务系统调用 (40-49) ========== */
#define SYS_TX_BEGIN    40    /* 开始事务 */
#define SYS_TX_COMMIT   41    /* 提交事务 */
#define SYS_TX_ABORT    42    /* 中止事务 */
#define SYS_TX_WRITE    43    /* 事务写入 */
#define SYS_TX_READ     44    /* 事务读取 */
#define SYS_TX_SNAPSHOT 45    /* 创建快照 */
#define SYS_TX_STATUS   46    /* 查询事务状态 */
#define SYS_TX_ROLLBACK 47    /* 回滚事务 */
#define SYS_TX_LOCK     48    /* 事务锁 */
#define SYS_TX_UNLOCK   49    /* 事务解锁 */

/* ========== 持久内存系统调用 (50-59) ========== */
#define SYS_PMEM_CREATE     50    /* 创建持久段 */
#define SYS_PMEM_OPEN       51    /* 打开持久段 */
#define SYS_PMEM_CLOSE      52    /* 关闭持久段 */
#define SYS_PMEM_ALLOC      53    /* 持久分配 */
#define SYS_PMEM_FREE       54    /* 持久释放 */
#define SYS_PMEM_SNAPSHOT   55    /* 创建快照 */
#define SYS_PMEM_RESTORE    56    /* 恢复快照 */
#define SYS_PMEM_SYNC       57    /* 同步到持久存储 */
#define SYS_PMEM_STAT       58    /* 获取统计信息 */
#define SYS_PMEM_LIST       59    /* 列出持久段 */

/* ========== 能力安全系统调用 (60-69) - 已废弃，使用新的 Capability 系统 ========== */
/* 旧的 capability syscall 已移除，请使用新的 SYS_CAPABILITY_BASE (170-179) */

/* ========== Z-Pipe 系统调用 (70-79) ========== */
#define SYS_ZPIPE_CREATE    70    /* 创建零拷贝管道 */
#define SYS_ZPIPE_DESTROY   71    /* 销毁管道 */
#define SYS_ZPIPE_READ      72    /* 零拷贝读 */
#define SYS_ZPIPE_WRITE     73    /* 零拷贝写 */
#define SYS_ZPIPE_SG_READ   74    /* Scatter-Gather 读 */
#define SYS_ZPIPE_SG_WRITE  75    /* Scatter-Gather 写 */
#define SYS_ZPIPE_MMAP      76    /* 映射管道缓冲区 */
#define SYS_ZPIPE_STAT      77    /* 获取管道统计 */
#define SYS_ZPIPE_BACKPRESS 78    /* 设置背压 */
#define SYS_ZPIPE_FLUSH     79    /* 刷新管道 */

/* ========== 自愈内核系统调用 (80-89) ========== */
#define SYS_SH_REGISTER     80    /* 注册故障处理器 */
#define SYS_SH_UNREGISTER   81    /* 注销处理器 */
#define SYS_SH_CHECKPOINT   82    /* 创建检查点 */
#define SYS_SH_RESTORE      83    /* 恢复检查点 */
#define SYS_SH_HEARTBEAT    84    /* 发送心跳 */
#define SYS_SH_WATCHDOG     85    /* 配置看门狗 */
#define SYS_SH_ISOLATE      86    /* 隔离故障域 */
#define SYS_SH_RECOVER      87    /* 触发恢复 */
#define SYS_SH_STATUS       88    /* 查询状态 */
#define SYS_SH_STAT         89    /* 获取统计 */

/* ========== 协程系统调用 (90-99) ========== */
#define SYS_CO_CREATE       90    /* 创建协程 */
#define SYS_CO_DESTROY      91    /* 销毁协程 */
#define SYS_CO_YIELD        92    /* 让出执行 */
#define SYS_CO_RESUME       93    /* 恢复执行 */
#define SYS_CO_JOIN         94    /* 等待协程 */
#define SYS_CO_CHAN_CREATE  95    /* 创建通道 */
#define SYS_CO_CHAN_SEND    96    /* 通道发送 */
#define SYS_CO_CHAN_RECV    97    /* 通道接收 */
#define SYS_CO_MUTEX_LOCK   98    /* 协程互斥锁 */
#define SYS_CO_MUTEX_UNLOCK 99    /* 协程解锁 */

/* ========== 异构计算系统调用 (100-109) ========== */
#define SYS_HET_INIT        100   /* 初始化异构调度器 */
#define SYS_HET_REG_DEV     101   /* 注册设备 */
#define SYS_HET_UNREG_DEV   102   /* 注销设备 */
#define SYS_HET_ALLOC_MEM   103   /* 分配设备内存 */
#define SYS_HET_FREE_MEM    104   /* 释放设备内存 */
#define SYS_HET_SUBMIT      105   /* 提交任务 */
#define SYS_HET_WAIT        106   /* 等待任务完成 */
#define SYS_HET_SELECT      107   /* 选择最优设备 */
#define SYS_HET_QUERY       108   /* 查询设备能力 */
#define SYS_HET_STAT        109   /* 获取统计 */

/* ========== 网络零拷贝系统调用 (110-119) ========== */
#define SYS_NZC_ENABLE      110   /* 启用零拷贝 */
#define SYS_NZC_DISABLE     111   /* 禁用零拷贝 */
#define SYS_NZC_SEND        112   /* 零拷贝发送 */
#define SYS_NZC_RECV        113   /* 零拷贝接收 */
#define SYS_NZC_SENDMSG     114   /* 零拷贝发送消息 */
#define SYS_NZC_RECVMSG     115   /* 零拷贝接收消息 */
#define SYS_NZC_SENDMMSG    116   /* 批量发送 */
#define SYS_NZC_RECVMMSG    117   /* 批量接收 */
#define SYS_NZC_REG_REGION  118   /* 注册内存区域 */
#define SYS_NZC_UNREG_REGION 119  /* 注销内存区域 */

/* ========== 实时调度系统调用 (120-129) ========== */
#define SYS_RT_CREATE       120   /* 创建实时任务 */
#define SYS_RT_DESTROY      121   /* 销毁实时任务 */
#define SYS_RT_SET_PARAM    122   /* 设置调度参数 */
#define SYS_RT_GET_PARAM    123   /* 获取调度参数 */
#define SYS_RT_SET_AFFINITY 124   /* 设置 CPU 亲和性 */
#define SYS_RT_YIELD        125   /* 实时让出 */
#define SYS_RT_DEADLINE     126   /* 设置截止时间 */
#define SYS_RT_PERIODIC     127   /* 注册周期任务 */
#define SYS_RT_STAT         128   /* 获取统计 */
#define SYS_RT_PI_BEGIN     129   /* 开始优先级继承 */

/* ========== 性能分析系统调用 (130-139) ========== */
#define SYS_PA_INIT         130   /* 初始化性能分析 */
#define SYS_PA_START        131   /* 开始分析 */
#define SYS_PA_STOP         132   /* 停止分析 */
#define SYS_PA_SAMPLE       133   /* 采样 */
#define SYS_PA_GET_REPORT   134   /* 获取报告 */
#define SYS_PA_REG_METRIC   135   /* 注册指标 */
#define SYS_PA_UPDATE       136   /* 更新指标 */
#define SYS_PA_GET_SUBSYS   137   /* 获取子系统统计 */
#define SYS_PA_FLAMEGRAPH   138   /* 获取火焰图 */
#define SYS_PA_RESET        139   /* 重置统计 */

/* ========== 资源限制系统调用 (140-149) ========== */
#define SYS_GETRLIMIT       140   /* 获取资源限制 */
#define SYS_SETRLIMIT       141   /* 设置资源限制 */
#define SYS_PRLIMIT         142   /* 进程资源限制 */
#define SYS_GETRUSAGE       143   /* 获取资源使用 */
#define SYS_SETPRIORITY     144   /* 设置优先级 */
#define SYS_GETPRIORITY     145   /* 获取优先级 */
#define SYS_SCHED_SETSCHEDULER 146 /* 设置调度策略 */
#define SYS_SCHED_GETSCHEDULER 147 /* 获取调度策略 */
#define SYS_SCHED_SETPARAM  148   /* 设置调度参数 */
#define SYS_SCHED_GETPARAM  149   /* 获取调度参数 */

/* ========== 内核测试系统调用 (150-159) ========== */
#define SYS_KTEST_RUN       150   /* 运行内核测试 */
#define SYS_KTEST_LIST      151   /* 列出测试 */
#define SYS_KTEST_RESULT    152   /* 获取测试结果 */
#define SYS_KTEST_BENCH     153   /* 运行基准测试 */
#define SYS_KTEST_STRESS    154   /* 运行压力测试 */
#define SYS_FB_SYNC         155   /* 同步用户空间 framebuffer */
#define SYS_GETFBINFO       156   /* 获取 framebuffer 信息 */

/* ========== PEXLF IPC 系统调用 (160-169) ========== */
#define SYS_PEXLF_INIT      160   /* 初始化 PEXLF */
#define SYS_PEXLF_CREATE    161   /* 创建通道 */
#define SYS_PEXLF_JOIN      162   /* 加入通道 */
#define SYS_PEXLF_LEAVE     163   /* 离开通道 */
#define SYS_PEXLF_SEND      164   /* 发送消息 */
#define SYS_PEXLF_RECV      165   /* 接收消息 */
#define SYS_PEXLF_BROADCAST 166   /* 广播消息 */
#define SYS_PEXLF_LIST      167   /* 列出通道 */
#define SYS_PEXLF_PEER_INFO 168   /* 获取对端信息 */

/* ========== Capability 安全模型系统调用 (170-179) ========== */
#define SYS_CAPABILITY_BASE 170
#define SYS_CAP_ALLOC       170   /* 分配 capability */
#define SYS_CAP_REVOKE      171   /* 撤销 capability */
#define SYS_CAP_CHECK       172   /* 检查权限 */
#define SYS_CAP_LIST        173   /* 列出 capabilities */
#define SYS_CAP_DELEGATE    174   /* 委托 capability */

/* ========== AI 自开发系统调用 (180-189) ========== */
#define SYS_AI_SEND         180   /* 发送消息给 AI */
#define SYS_AI_RECV         181   /* 接收 AI 响应 */
#define SYS_AI_STATUS       182   /* 查询 AI 服务状态 */
#define SYS_AI_CONFIG       183   /* 配置 AI 服务 */
#define SYS_AI_CANCEL       184   /* 取消进行中的 AI 请求 */
#define SYS_AI_LOG_READ     185   /* 读取对话日志 */
#define SYS_AI_LOG_WRITE    186   /* 写入对话日志 */
#define SYS_AI_LOG_LIST     187   /* 列出对话日志 */

/* 输入子系统 */
#define SYS_INPUT_POLL    188   /* 轮询键盘/鼠标事件 */

/* 权限系统调用 */
#define SYS_GETUID        190
#define SYS_GETGID        191
#define SYS_SETUID        192
#define SYS_SETGID        193
#define SYS_CHMOD         194
#define SYS_CHOWN         195
#define SYS_SUDO          196

#define SYS_MP4_OPEN      197
#define SYS_MP4_DECODE    198
#define SYS_MP4_CLOSE     199

/* ========== 网络套接字扩展 (200-209) ========== */
#define SYS_LISTEN          200   /* 监听连接 */
#define SYS_ACCEPT          201   /* 接受连接 */
#define SYS_CONNECT         202   /* 发起连接 */
#define SYS_SEND            203   /* 发送数据 */
#define SYS_RECV            204   /* 接收数据 */
#define SYS_SHUTDOWN        205   /* 关闭连接 */
#define SYS_GETSOCKOPT      206   /* 获取套接字选项 */
#define SYS_SETSOCKOPT      207   /* 设置套接字选项 */
#define SYS_GETPEERNAME     208   /* 获取对端地址 */
#define SYS_SENDTO          209   /* 发送到指定地址 */

/* ========== 系统调用参数结构 ==========*/

struct syscall_args {
    uint64_t num;       /* 系统调用号 */
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t arg4;
    uint64_t arg5;
};

/* 初始化系统调用 */
void syscall_init(void);

/* 系统调用处理程序 */
void syscall_handler(struct interrupt_frame *frame);

/* 用户态系统调用包装 */
static inline uint64_t syscall0(uint64_t num) {
    uint64_t ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );
    return ret;
}

static inline uint64_t syscall1(uint64_t num, uint64_t arg0) {
    uint64_t ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg0)
        : "memory"
    );
    return ret;
}

static inline uint64_t syscall2(uint64_t num, uint64_t arg0, uint64_t arg1) {
    uint64_t ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1)
        : "memory"
    );
    return ret;
}

static inline uint64_t syscall3(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = arg2;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1), "r"(r10)
        : "memory"
    );
    return ret;
}

static inline uint64_t syscall4(uint64_t num, uint64_t arg0, uint64_t arg1, 
                                 uint64_t arg2, uint64_t arg3) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = arg2;
    register uint64_t r8 __asm__("r8") = arg3;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1), "r"(r10), "r"(r8)
        : "memory"
    );
    return ret;
}

static inline uint64_t syscall5(uint64_t num, uint64_t arg0, uint64_t arg1,
                                 uint64_t arg2, uint64_t arg3, uint64_t arg4) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = arg2;
    register uint64_t r8 __asm__("r8") = arg3;
    register uint64_t r9 __asm__("r9") = arg4;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1), "r"(r10), "r"(r8), "r"(r9)
        : "memory"
    );
    return ret;
}

#endif
