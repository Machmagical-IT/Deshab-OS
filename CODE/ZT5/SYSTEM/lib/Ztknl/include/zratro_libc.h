#ifndef ZRATRO_LIBC_H
#define ZRATRO_LIBC_H

/*
 * Zratro C Library 适配层
 * 为 musl-libc 提供系统调用接口
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* 类型定义 (与 musl 兼容) */
typedef int32_t pid_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int32_t mode_t;
typedef uint64_t ino_t;
typedef uint64_t dev_t;
typedef int64_t blksize_t;
typedef int64_t blkcnt_t;
typedef int64_t off_t;
typedef int64_t time_t;
typedef int64_t suseconds_t;
typedef int32_t clockid_t;
typedef uint64_t pthread_t;
typedef uint32_t pthread_attr_t;
typedef uint32_t pthread_mutex_t;
typedef uint32_t pthread_cond_t;

/* 文件类型 */
#define S_IFMT      0170000
#define S_IFDIR     0040000
#define S_IFCHR     0020000
#define S_IFBLK     0060000
#define S_IFREG     0100000
#define S_IFIFO     0010000
#define S_IFLNK     0120000
#define S_IFSOCK    0140000
#define S_ISUID     04000
#define S_ISGID     02000
#define S_ISVTX     01000
#define S_IRWXU     0700
#define S_IRUSR     0400
#define S_IWUSR     0200
#define S_IXUSR     0100
#define S_IRWXG     070
#define S_IRGRP     040
#define S_IWGRP     020
#define S_IXGRP     010
#define S_IRWXO     07
#define S_IROTH     04
#define S_IWOTH     02
#define S_IXOTH     01

/* stat 结构 (与 musl 兼容) */
struct stat {
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    blksize_t st_blksize;
    blkcnt_t st_blocks;
    
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};

/* 时间结构 */
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};

/* 时钟类型 */
#define CLOCK_REALTIME              0
#define CLOCK_MONOTONIC             1
#define CLOCK_PROCESS_CPUTIME_ID    2
#define CLOCK_THREAD_CPUTIME_ID     3
#define CLOCK_MONOTONIC_RAW         4
#define CLOCK_REALTIME_COARSE       5
#define CLOCK_MONOTONIC_COARSE      6

/* open 标志 */
#define O_RDONLY        00
#define O_WRONLY        01
#define O_RDWR          02
#define O_CREAT         0100
#define O_EXCL          0200
#define O_NOCTTY        0400
#define O_TRUNC         01000
#define O_APPEND        02000
#define O_NONBLOCK      04000
#define O_DSYNC         010000
#define O_SYNC          04010000
#define O_RSYNC         04010000
#define O_DIRECTORY     0200000
#define O_NOFOLLOW      0400000
#define O_CLOEXEC       02000000
#define O_ASYNC         020000
#define O_DIRECT        040000
#define O_LARGEFILE     0
#define O_NOATIME       01000000
#define O_PATH          010000000
#define O_TMPFILE       020200000

/* mmap 标志 */
#define PROT_NONE       0
#define PROT_READ       1
#define PROT_WRITE      2
#define PROT_EXEC       4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_GROWSDOWN   0x0100
#define MAP_DENYWRITE   0x0800
#define MAP_EXECUTABLE  0x1000
#define MAP_LOCKED      0x2000
#define MAP_NORESERVE   0x4000
#define MAP_POPULATE    0x8000
#define MAP_NONBLOCK    0x10000
#define MAP_STACK       0x20000
#define MAP_HUGETLB     0x40000

/* errno 值 */
#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EINTR           4
#define EIO             5
#define ENXIO           6
#define E2BIG           7
#define ENOEXEC         8
#define EBADF           9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define ENOTBLK         15
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define ETXTBSY         26
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EMLINK          31
#define EPIPE           32
#define EDOM            33
#define ERANGE          34
#define EDEADLK         35
#define ENAMETOOLONG    36
#define ENOLCK          37
#define ENOSYS          38
#define ENOTEMPTY       39
#define ELOOP           40
#define EWOULDBLOCK     EAGAIN
#define ENOMSG          42
#define EIDRM           43
#define ECHRNG          44
#define EL2NSYNC        45
#define EL3HLT          46
#define EL3RST          47
#define ELNRNG          48
#define EUNATCH         49
#define ENOCSI          50

/* 信号 */
#define SIGHUP          1
#define SIGINT          2
#define SIGQUIT         3
#define SIGILL          4
#define SIGTRAP         5
#define SIGABRT         6
#define SIGIOT          SIGABRT
#define SIGBUS          7
#define SIGFPE          8
#define SIGKILL         9
#define SIGUSR1         10
#define SIGSEGV         11
#define SIGUSR2         12
#define SIGPIPE         13
#define SIGALRM         14
#define SIGTERM         15
#define SIGSTKFLT       16
#define SIGCHLD         17
#define SIGCONT         18
#define SIGSTOP         19
#define SIGTSTP         20
#define SIGTTIN         21
#define SIGTTOU         22
#define SIGURG          23
#define SIGXCPU         24
#define SIGXFSZ         25
#define SIGVTALRM       26
#define SIGPROF         27
#define SIGWINCH        28
#define SIGIO           29
#define SIGPOLL         SIGIO
#define SIGPWR          30
#define SIGSYS          31

/* sigaction 标志 */
#define SA_NOCLDSTOP    1
#define SA_NOCLDWAIT    2
#define SA_SIGINFO      4
#define SA_ONSTACK      0x08000000
#define SA_RESTART      0x10000000
#define SA_NODEFER      0x40000000
#define SA_RESETHAND    0x80000000

/* sigset_t */
typedef struct {
    uint32_t sig[2];
} sigset_t;

/* sigaction 结构 */
struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, void *, void *);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

/* 进程调度 */
#define SCHED_OTHER      0
#define SCHED_FIFO       1
#define SCHED_RR         2
#define SCHED_BATCH      3
#define SCHED_IDLE       5
#define SCHED_DEADLINE   6

struct sched_param {
    int sched_priority;
};

/* sysctl */
struct sysctl_args {
    int *name;
    int nlen;
    void *oldval;
    size_t *oldlenp;
    void *newval;
    size_t newlen;
    unsigned long unused[4];
};

/* Zratro 系统调用接口 */
long zratro_syscall(long num, ...);

/* 系统调用包装函数 */
int zratro_open(const char *path, int flags, mode_t mode);
int zratro_close(int fd);
ssize_t zratro_read(int fd, void *buf, size_t count);
ssize_t zratro_write(int fd, const void *buf, size_t count);
off_t zratro_lseek(int fd, off_t offset, int whence);
int zratro_stat(const char *path, struct stat *buf);
int zratro_fstat(int fd, struct stat *buf);
int zratro_mkdir(const char *path, mode_t mode);
int zratro_rmdir(const char *path);
int zratro_unlink(const char *path);
int zratro_link(const char *oldpath, const char *newpath);
int zratro_rename(const char *oldpath, const char *newpath);
int zratro_chmod(const char *path, mode_t mode);
int zratro_chown(const char *path, uid_t owner, gid_t group);

void *zratro_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int zratro_munmap(void *addr, size_t length);
int zratro_mprotect(void *addr, size_t len, int prot);
int zratro_msync(void *addr, size_t length, int flags);

pid_t zratro_fork(void);
pid_t zratro_vfork(void);
int zratro_execve(const char *path, char *const argv[], char *const envp[]);
void zratro_exit(int status);
pid_t zratro_waitpid(pid_t pid, int *status, int options);
pid_t zratro_getpid(void);
pid_t zratro_getppid(void);
uid_t zratro_getuid(void);
gid_t zratro_getgid(void);

int zratro_pipe(int pipefd[2]);
int zratro_dup(int oldfd);
int zratro_dup2(int oldfd, int newfd);
int zratro_dup3(int oldfd, int newfd, int flags);

int zratro_socket(int domain, int type, int protocol);
int zratro_bind(int sockfd, const void *addr, socklen_t addrlen);
int zratro_listen(int sockfd, int backlog);
int zratro_accept(int sockfd, void *addr, socklen_t *addrlen);
int zratro_connect(int sockfd, const void *addr, socklen_t addrlen);
ssize_t zratro_send(int sockfd, const void *buf, size_t len, int flags);
ssize_t zratro_recv(int sockfd, void *buf, size_t len, int flags);

int zratro_kill(pid_t pid, int sig);
int zratro_sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
int zratro_sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int zratro_sigpending(sigset_t *set);
int zratro_sigsuspend(const sigset_t *mask);

int zratro_nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int zratro_alarm(unsigned int seconds);
int zratro_clock_gettime(clockid_t clk_id, struct timespec *tp);
int zratro_clock_settime(clockid_t clk_id, const struct timespec *tp);
int zratro_gettimeofday(struct timeval *tv, void *tz);

int zratro_sched_yield(void);
int zratro_sched_getparam(pid_t pid, struct sched_param *param);
int zratro_sched_setparam(pid_t pid, const struct sched_param *param);

int zratro_ioctl(int fd, unsigned long request, ...);
int zratro_fcntl(int fd, int cmd, ...);

char *zratro_getcwd(char *buf, size_t size);
int zratro_chdir(const char *path);
int zratro_fchdir(int fd);

long zratro_sysconf(int name);
int zratro_uname(void *buf);

#endif
