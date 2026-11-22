#ifndef ABI_H
#define ABI_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"   /* 引入统一的 pid_t, uid_t, gid_t, off_t, time_t 定义 */

/* ABI 类型标识 */
typedef enum {
    ABI_UNKNOWN = 0,
    ABI_ZRATRO,         /* Zratro 原生 ABI */
    ABI_LINUX,          /* Linux x86_64 ABI */
    ABI_FREEBSD,        /* 保留: FreeBSD */
    ABI_WINDOWS,        /* Windows NT ABI (PE) */
} abi_type_t;

/* ========== Linux x86_64 系统调用号 ========== */

/* 文件操作 */
#define LINUX_SYS_READ          0
#define LINUX_SYS_WRITE         1
#define LINUX_SYS_OPEN          2
#define LINUX_SYS_CLOSE         3
#define LINUX_SYS_STAT          4
#define LINUX_SYS_FSTAT         5
#define LINUX_SYS_LSTAT         6
#define LINUX_SYS_POLL          7
#define LINUX_SYS_LSEEK         8
#define LINUX_SYS_MMAP          9
#define LINUX_SYS_MPROTECT      10
#define LINUX_SYS_MUNMAP        11
#define LINUX_SYS_BRK           12
#define LINUX_SYS_IOCTL         16
#define LINUX_SYS_PREAD64       17
#define LINUX_SYS_PWRITE64      18
#define LINUX_SYS_READV         19
#define LINUX_SYS_WRITEV        20
#define LINUX_SYS_ACCESS        21
#define LINUX_SYS_PIPE          22
#define LINUX_SYS_SELECT        23
#define LINUX_SYS_DUP           32
#define LINUX_SYS_DUP2          33
#define LINUX_SYS_FCNTL         72
#define LINUX_SYS_GETDENTS      78
#define LINUX_SYS_GETDENTS64    217
#define LINUX_SYS_GETCWD        79
#define LINUX_SYS_CHDIR         80
#define LINUX_SYS_FCHDIR        81
#define LINUX_SYS_RENAME        82
#define LINUX_SYS_MKDIR         83
#define LINUX_SYS_RMDIR         84
#define LINUX_SYS_CREAT         85
#define LINUX_SYS_LINK          86
#define LINUX_SYS_UNLINK        87
#define LINUX_SYS_SYMLINK       88
#define LINUX_SYS_READLINK      89
#define LINUX_SYS_CHMOD         90
#define LINUX_SYS_FCHMOD        91
#define LINUX_SYS_CHOWN         92
#define LINUX_SYS_FCHOWN        93
#define LINUX_SYS_LCHOWN        94
#define LINUX_SYS_UMASK         95
#define LINUX_SYS_TRUNCATE      76
#define LINUX_SYS_FTRUNCATE     77

/* 进程管理 */
#define LINUX_SYS_SCHED_YIELD   24
#define LINUX_SYS_GETPID        39
#define LINUX_SYS_GETPPID       110
#define LINUX_SYS_GETUID        102
#define LINUX_SYS_GETGID        104
#define LINUX_SYS_GETEUID       107
#define LINUX_SYS_GETEGID       108
#define LINUX_SYS_SETUID        105
#define LINUX_SYS_SETGID        106
#define LINUX_SYS_GETTID        186
#define LINUX_SYS_CLONE         56
#define LINUX_SYS_FORK          57
#define LINUX_SYS_VFORK         58
#define LINUX_SYS_EXECVE        59
#define LINUX_SYS_EXIT          60
#define LINUX_SYS_WAIT4         61
#define LINUX_SYS_EXIT_GROUP    231
#define LINUX_SYS_WAITID        247
#define LINUX_SYS_SET_TID_ADDRESS 218
#define LINUX_SYS_SET_ROBUST_LIST 273
#define LINUX_SYS_GET_ROBUST_LIST 274
#define LINUX_SYS_ARCH_PRCTL    158

/* 信号处理 */
#define LINUX_SYS_RT_SIGACTION  13
#define LINUX_SYS_RT_SIGPROCMASK 14
#define LINUX_SYS_RT_SIGRETURN  15
#define LINUX_SYS_SIGALTSTACK   131
#define LINUX_SYS_KILL          62
#define LINUX_SYS_TGKILL        234
#define LINUX_SYS_TKILL         200
#define LINUX_SYS_PAUSE         34
#define LINUX_SYS_SIGSUSPEND    130
#define LINUX_SYS_SIGPENDING    127

/* 时间 */
#define LINUX_SYS_NANOSLEEP     35
#define LINUX_SYS_CLOCK_GETTIME 228
#define LINUX_SYS_CLOCK_SETTIME 227
#define LINUX_SYS_CLOCK_GETRES  229
#define LINUX_SYS_GETTIMEOFDAY  96
#define LINUX_SYS_SETTIMEOFDAY  164
#define LINUX_SYS_TIME          201
#define LINUX_SYS_ALARM         37
#define LINUX_SYS_GETITIMER     36
#define LINUX_SYS_SETITIMER     38

/* 内存管理 */
#define LINUX_SYS_MREMAP        25
#define LINUX_SYS_MSYNC         26
#define LINUX_SYS_MINCORE       27
#define LINUX_SYS_MADVISE       28
#define LINUX_SYS_SHMGET        29
#define LINUX_SYS_SHMAT         30
#define LINUX_SYS_SHMCTL        31
#define LINUX_SYS_GETRLIMIT     97
#define LINUX_SYS_SETRLIMIT     160
#define LINUX_SYS_GETRUSAGE     98
#define LINUX_SYS_PRLIMIT64     302

/* 网络 */
#define LINUX_SYS_SOCKET        41
#define LINUX_SYS_CONNECT       42
#define LINUX_SYS_ACCEPT        43
#define LINUX_SYS_SENDTO        44
#define LINUX_SYS_RECVFROM      45
#define LINUX_SYS_SENDMSG       46
#define LINUX_SYS_RECVMSG       47
#define LINUX_SYS_SHUTDOWN      48
#define LINUX_SYS_BIND          49
#define LINUX_SYS_LISTEN        50
#define LINUX_SYS_GETSOCKNAME   51
#define LINUX_SYS_GETPEERNAME   52
#define LINUX_SYS_SOCKETPAIR    53
#define LINUX_SYS_SETSOCKOPT    54
#define LINUX_SYS_GETSOCKOPT    55
#define LINUX_SYS_ACCEPT4       288

/* 同步 */
#define LINUX_SYS_FUTEX         202
#define LINUX_SYS_EPOLL_CREATE  213
#define LINUX_SYS_EPOLL_CTL     233
#define LINUX_SYS_EPOLL_WAIT    232
#define LINUX_SYS_EPOLL_CREATE1 291
#define LINUX_SYS_EPOLL_PWAIT   281
#define LINUX_SYS_EVENTFD2      290
#define LINUX_SYS_TIMERFD_CREATE 283
#define LINUX_SYS_TIMERFD_SETTIME 286
#define LINUX_SYS_TIMERFD_GETTIME 287
#define LINUX_SYS_SIGNALFD4     289
#define LINUX_SYS_PIPE2         293

/* 其他 */
#define LINUX_SYS_UNAME         63
#define LINUX_SYS_UMOUNT2       39
#define LINUX_SYS_MOUNT         165
#define LINUX_SYS_GETRANDOM     318
#define LINUX_SYS_MEMFD_CREATE  319
#define LINUX_SYS_COPY_FILE_RANGE 326
#define LINUX_SYS_SPLICE        275
#define LINUX_SYS_TEE           276
#define LINUX_SYS_VSPLICE       278
#define LINUX_SYS_KCMP          312
#define LINUX_SYS_SENDFILE      40

/* ========== Linux 标志定义 ========== */

/* mmap 保护标志 */
#define LINUX_PROT_NONE         0x0
#define LINUX_PROT_READ         0x1
#define LINUX_PROT_WRITE        0x2
#define LINUX_PROT_EXEC         0x4
#define LINUX_PROT_GROWSDOWN    0x01000000
#define LINUX_PROT_GROWSUP      0x02000000

/* mmap 标志 */
#define LINUX_MAP_SHARED        0x01
#define LINUX_MAP_PRIVATE       0x02
#define LINUX_MAP_FIXED         0x10
#define LINUX_MAP_ANONYMOUS     0x20
#define LINUX_MAP_GROWSDOWN     0x0100
#define LINUX_MAP_DENYWRITE     0x0800
#define LINUX_MAP_EXECUTABLE    0x1000
#define LINUX_MAP_LOCKED        0x2000
#define LINUX_MAP_NORESERVE     0x4000
#define LINUX_MAP_POPULATE      0x8000
#define LINUX_MAP_NONBLOCK      0x10000
#define LINUX_MAP_STACK         0x20000
#define LINUX_MAP_HUGETLB       0x40000

/* open 标志 */
#define LINUX_O_RDONLY          0
#define LINUX_O_WRONLY          1
#define LINUX_O_RDWR            2
#define LINUX_O_CREAT           0100
#define LINUX_O_EXCL            0200
#define LINUX_O_NOCTTY          0400
#define LINUX_O_TRUNC           01000
#define LINUX_O_APPEND          02000
#define LINUX_O_NONBLOCK        04000
#define LINUX_O_DSYNC           010000
#define LINUX_O_SYNC            04010000
#define LINUX_O_RSYNC           04010000
#define LINUX_O_DIRECTORY       0200000
#define LINUX_O_NOFOLLOW        0400000
#define LINUX_O_CLOEXEC         02000000
#define LINUX_O_ASYNC           020000

/* 文件模式 */
#define LINUX_S_IFMT            0170000
#define LINUX_S_IFSOCK          0140000
#define LINUX_S_IFLNK           0120000
#define LINUX_S_IFREG           0100000
#define LINUX_S_IFBLK           0060000
#define LINUX_S_IFDIR           0040000
#define LINUX_S_IFCHR           0020000
#define LINUX_S_IFIFO           0010000
#define LINUX_S_ISUID           0004000
#define LINUX_S_ISGID           0002000
#define LINUX_S_ISVTX           0001000
#define LINUX_S_IRWXU           00700
#define LINUX_S_IRUSR           00400
#define LINUX_S_IWUSR           00200
#define LINUX_S_IXUSR           00100
#define LINUX_S_IRWXG           00070
#define LINUX_S_IRGRP           00040
#define LINUX_S_IWGRP           00020
#define LINUX_S_IXGRP           00010
#define LINUX_S_IRWXO           00007
#define LINUX_S_IROTH           00004
#define LINUX_S_IWOTH           00002
#define LINUX_S_IXOTH           00001

/* futex 操作 */
#define LINUX_FUTEX_WAIT        0
#define LINUX_FUTEX_WAKE        1
#define LINUX_FUTEX_FD          2
#define LINUX_FUTEX_REQUEUE     3
#define LINUX_FUTEX_CMP_REQUEUE 4
#define LINUX_FUTEX_WAKE_OP     5
#define LINUX_FUTEX_LOCK_PI     6
#define LINUX_FUTEX_UNLOCK_PI   7
#define LINUX_FUTEX_TRYLOCK_PI  8
#define LINUX_FUTEX_WAIT_BITSET 9
#define LINUX_FUTEX_WAKE_BITSET 10
#define LINUX_FUTEX_WAIT_REQUEUE_PI  11
#define LINUX_FUTEX_CMP_REQUEUE_PI   12
#define LINUX_FUTEX_PRIVATE_FLAG  128

/* clone 标志 */
#define LINUX_CLONE_VM          0x00000100
#define LINUX_CLONE_FS          0x00000200
#define LINUX_CLONE_FILES       0x00000400
#define LINUX_CLONE_SIGHAND     0x00000800
#define LINUX_CLONE_PTRACE      0x00002000
#define LINUX_CLONE_VFORK       0x00004000
#define LINUX_CLONE_PARENT      0x00008000
#define LINUX_CLONE_THREAD      0x00010000
#define LINUX_CLONE_NEWNS       0x00020000
#define LINUX_CLONE_SYSVSEM     0x00040000
#define LINUX_CLONE_SETTLS      0x00080000
#define LINUX_CLONE_PARENT_SETTID  0x00100000
#define LINUX_CLONE_CHILD_CLEARTID 0x00200000
#define LINUX_CLONE_CHILD_SETTID   0x01000000

/* arch_prctl 操作 */
#define LINUX_ARCH_SET_FS       0x1002
#define LINUX_ARCH_GET_FS       0x1003
#define LINUX_ARCH_SET_GS       0x1001
#define LINUX_ARCH_GET_GS       0x1004

/* ========== ELF ABI 识别 ========== */

#define EI_OSABI        7
#define ELFOSABI_NONE           0       /* UNIX System V ABI */
#define ELFOSABI_LINUX          3       /* Linux */
#define ELFOSABI_FREEBSD        9       /* FreeBSD */

/* ========== 函数声明 ========== */

/* Linux syscall 处理器 */
uint64_t linux_syscall_handler(uint64_t num, uint64_t arg0, uint64_t arg1,
                               uint64_t arg2, uint64_t arg3, uint64_t arg4,
                               uint64_t arg5);

/* ABI 检测 */
abi_type_t abi_detect_from_elf(const uint8_t *elf_data);

/* ABI 初始化 */
int abi_init(void);

/* ABI 名称 */
const char *abi_get_name(abi_type_t abi);

/* 获取进程 ABI */
abi_type_t abi_get_process_abi(uint32_t pid);

/* 设置进程 ABI */
void abi_set_process_abi(uint32_t pid, abi_type_t abi);

#endif
