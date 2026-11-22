#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"   /* 引入统一的 ssize_t 等类型定义 */

/* 类型定义 */
typedef size_t socklen_t;
typedef unsigned long nfds_t;

/* 地址族 */
#define AF_UNSPEC       0
#define AF_UNIX         1       /* Unix域套接字 */
#define AF_INET         2       /* IPv4 */
#define AF_AX25         3
#define AF_IPX          4
#define AF_APPLETALK    5
#define AF_NETROM       6
#define AF_BRIDGE       7
#define AF_ATMPVC       8
#define AF_X25          9
#define AF_INET6        10      /* IPv6 */
#define AF_ROSE         11
#define AF_DECnet       12
#define AF_NETBEUI      13
#define AF_SECURITY     14
#define AF_KEY          15
#define AF_NETLINK      16
#define AF_PACKET       17
#define AF_BLUETOOTH    31

/* 套接字类型 */
#define SOCK_STREAM     1       /* 面向连接的流 (TCP) */
#define SOCK_DGRAM      2       /* 无连接数据报 (UDP) */
#define SOCK_RAW        3       /* 原始套接字 */
#define SOCK_RDM        4
#define SOCK_SEQPACKET  5
#define SOCK_DCCP       6
#define SOCK_PACKET     10

/* 协议 */
#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_IGMP    2
#define IPPROTO_IPIP    4
#define IPPROTO_TCP     6
#define IPPROTO_EGP     8
#define IPPROTO_PUP     12
#define IPPROTO_UDP     17
#define IPPROTO_IDP     22
#define IPPROTO_DCCP    33
#define IPPROTO_IPV6    41
#define IPPROTO_ROUTING 43
#define IPPROTO_FRAGMENT 44
#define IPPROTO_ESP     50
#define IPPROTO_AH      51
#define IPPROTO_ICMPV6  58
#define IPPROTO_NONE    59
#define IPPROTO_DSTOPTS 60
#define IPPROTO_MTP     92
#define IPPROTO_ENCAP   98
#define IPPROTO_PIM     103
#define IPPROTO_COMP    108
#define IPPROTO_SCTP    132
#define IPPROTO_UDPLITE 136
#define IPPROTO_RAW     255

/* 套接字选项级别 */
#define SOL_SOCKET      1
#define SOL_IP          0
#define SOL_IPV6        41
#define SOL_TCP         6
#define SOL_UDP         17

/* TCP 套接字选项 */
#define TCP_NODELAY     1       /* 禁用 Nagle 算法 */
#define TCP_MAXSEG      2       /* 设置 MSS */
#define TCP_KEEPIDLE    4       /* Keep-Alive 空闲时间 (秒) */
#define TCP_KEEPINTVL   5       /* Keep-Alive 探测间隔 (秒) */
#define TCP_KEEPCNT     6       /* Keep-Alive 最大探测次数 */
#define TCP_INFO        11      /* 获取 TCP 信息 */

/* 套接字选项 */
#define SO_DEBUG        1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_DONTROUTE    5
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_SNDBUFFORCE  32
#define SO_RCVBUFFORCE  33
#define SO_KEEPALIVE    9
#define SO_OOBINLINE    10
#define SO_NO_CHECK     11
#define SO_PRIORITY     12
#define SO_LINGER       13
#define SO_BSDCOMPAT    14
#define SO_REUSEPORT    15
#define SO_PASSCRED     16
#define SO_PEERCRED     17
#define SO_RCVLOWAT     18
#define SO_SNDLOWAT     19
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_BINDTODEVICE 25
#define SO_ATTACH_FILTER 26
#define SO_DETACH_FILTER 27
#define SO_PEERNAME     28
#define SO_ACCEPTCONN   30
#define SO_PEERSEC      31
#define SO_PASSSEC      34
#define SO_TIMESTAMP    29
#define SO_TIMESTAMPNS  35
#define SO_MARK         36
#define SO_TIMESTAMPING 37
#define SO_PROTOCOL     38
#define SO_DOMAIN       39

/* 标志 */
#define MSG_OOB         0x0001
#define MSG_PEEK        0x0002
#define MSG_DONTROUTE   0x0004
#define MSG_TRYHARD     0x0004
#define MSG_CTRUNC      0x0008
#define MSG_PROBE       0x0010
#define MSG_TRUNC       0x0020
#define MSG_DONTWAIT    0x0040
#define MSG_EOR         0x0080
#define MSG_WAITALL     0x0100
#define MSG_FIN         0x0200
#define MSG_SYN         0x0400
#define MSG_CONFIRM     0x0800
#define MSG_RST         0x1000
#define MSG_ERRQUEUE    0x2000
#define MSG_NOSIGNAL    0x4000
#define MSG_MORE        0x8000
#define MSG_WAITFORONE  0x10000
#define MSG_BATCH       0x40000
#define MSG_ZEROCOPY    0x4000000

/* shutdown 参数 */
#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

/* IPv4 地址结构 */
struct in_addr {
    uint32_t s_addr;
};

/* IPv4 套接字地址 */
struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

/* IPv6 地址结构 */
struct in6_addr {
    uint8_t s6_addr[16];
};

/* IPv6 套接字地址 */
struct sockaddr_in6 {
    uint16_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

/* 通用套接字地址 */
struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

/* 套接字地址存储 */
struct sockaddr_storage {
    uint16_t ss_family;
    char __ss_pad1[6];
    uint64_t __ss_align;
    char __ss_pad2[112];
};

/* 套接字结构 */
struct socket {
    int domain;
    int type;
    int protocol;
    int state;
    int flags;
    
    /* O_NONBLOCK 标志 */
    int nonblock;
    
    /* 套接字选项 */
    int so_reuseaddr;
    int so_keepalive;
    int so_broadcast;
    int so_oobinline;
    size_t so_rcvbuf;
    size_t so_sndbuf;
    int so_error;
    int so_acceptconn;
    
    /* TCP 特定选项 */
    int tcp_nodelay;
    int tcp_keepidle;         /* Keep-Alive 空闲时间 (秒) */
    int tcp_keepintvl;        /* Keep-Alive 探测间隔 (秒) */
    int tcp_keepcnt;          /* Keep-Alive 最大探测次数 */
    
    /* 绑定地址 */
    struct sockaddr_storage local_addr;
    socklen_t local_addrlen;
    
    /* 远端地址 */
    struct sockaddr_storage remote_addr;
    socklen_t remote_addrlen;
    
    /* 缓冲区 */
    void *recv_buf;
    size_t recv_buf_size;
    size_t recv_buf_len;
    
    void *send_buf;
    size_t send_buf_size;
    size_t send_buf_len;
    
    /* 等待队列 */
    int waiting_threads;
    
    /* 引用计数 */
    int ref_count;

    /* For socketpair: only send_buf is owned; recv_buf aliases peer's send_buf */
    int owns_recv_buf;
    
    /* Unix 域套接字路径 */
    char unix_path[108];
    
    /* Unix 域套接字: 连接的对端 fd */
    int peer_fd;
};

/* 套接字状态 */
#define SS_UNCONNECTED      0
#define SS_CONNECTING       1
#define SS_CONNECTED        2
#define SS_DISCONNECTING    3

/* 获取套接字结构 */
struct socket *get_socket(int sockfd);

/* 初始化网络子系统 */
int socket_init(void);

/* 创建套接字 */
int sys_socket(int domain, int type, int protocol);

/* 绑定地址 */
int sys_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

/* 监听连接 */
int sys_listen(int sockfd, int backlog);

/* 接受连接 */
int sys_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

/* 发起连接 */
int sys_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

/* 发送数据 */
ssize_t sys_send(int sockfd, const void *buf, size_t len, int flags);
ssize_t sys_sendto(int sockfd, const void *buf, size_t len, int flags,
                   const struct sockaddr *dest_addr, socklen_t addrlen);

/* 接收数据 */
ssize_t sys_recv(int sockfd, void *buf, size_t len, int flags);
ssize_t sys_recvfrom(int sockfd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen);

/* 关闭连接 */
int sys_shutdown(int sockfd, int how);
int sys_closesocket(int sockfd);

/* 获取/设置套接字选项 */
int sys_getsockopt(int sockfd, int level, int optname,
                   void *optval, socklen_t *optlen);
int sys_setsockopt(int sockfd, int level, int optname,
                   const void *optval, socklen_t optlen);

/* 获取套接字地址 */
int sys_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int sys_getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

/* 套接字对 */
int sys_socketpair(int domain, int type, int protocol, int sv[2]);

/* poll/select 支持 */
#define POLLIN          0x0001
#define POLLPRI         0x0002
#define POLLOUT         0x0004
#define POLLERR         0x0008
#define POLLHUP         0x0010
#define POLLNVAL        0x0020

struct pollfd {
    int fd;
    short events;
    short revents;
};

int sys_poll(struct pollfd *fds, nfds_t nfds, int timeout);

/* select 支持 */
#define FD_SETSIZE      1024

typedef struct {
    uint8_t fds_bits[FD_SETSIZE / 8];
} fd_set;

#define FD_ZERO(set)    memset((set), 0, sizeof(*(set)))
#define FD_SET(fd, set) ((set)->fds_bits[(fd) / 8] |= (1 << ((fd) % 8)))
#define FD_CLR(fd, set) ((set)->fds_bits[(fd) / 8] &= ~(1 << ((fd) % 8)))
#define FD_ISSET(fd, set) ((set)->fds_bits[(fd) / 8] & (1 << ((fd) % 8)))

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

int sys_select(int nfds, fd_set *readfds, fd_set *writefds, 
               fd_set *exceptfds, struct timeval *timeout);

/* O_NONBLOCK 标志 (用于 sys_socket 的 type 参数) */
#define O_NONBLOCK      0x0800

/* 非阻塞 I/O 错误码 */
#define EAGAIN          11      /* 资源暂时不可用 */
#define EWOULDBLOCK     11      /* 操作会阻塞 (与 EAGAIN 相同) */
#define EINPROGRESS     36      /* 操作正在进行 */

/* Unix 域套接字地址结构 */
struct sockaddr_un {
    uint16_t sun_family;        /* AF_UNIX */
    char sun_path[108];         /* 路径名 */
};

#endif
