#include "socket.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "task.h"

/* 套接字表 */
#define MAX_SOCKETS     1024
static struct socket socket_table[MAX_SOCKETS];
static int socket_count = 0;

/* 初始化网络子系统 */
int socket_init(void) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        socket_table[i].state = -1;  /* 未使用 */
        socket_table[i].ref_count = 0;
    }
    socket_count = 0;
    return 0;
}

/* 分配套接字 */
static int alloc_socket(void) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (socket_table[i].ref_count == 0) {
            if (i >= socket_count) {
                socket_count = i + 1;
            }
            return i;
        }
    }
    return -1;  /* EMFILE */
}

/* 获取套接字 */
struct socket *get_socket(int sockfd) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) {
        return NULL;
    }
    if (socket_table[sockfd].ref_count == 0) {
        return NULL;
    }
    return &socket_table[sockfd];
}

/* 创建套接字 */
int sys_socket(int domain, int type, int protocol) {
    int fd = alloc_socket();
    if (fd < 0) {
        return -1;  /* EMFILE */
    }
    
    struct socket *sock = &socket_table[fd];
    
    /* 检查 O_NONBLOCK 标志 */
    int nonblock = 0;
    if (type & O_NONBLOCK) {
        nonblock = 1;
        type &= ~O_NONBLOCK;
    }
    
    sock->domain = domain;
    sock->type = type;
    sock->protocol = protocol;
    sock->state = SS_UNCONNECTED;
    sock->flags = 0;
    sock->nonblock = nonblock;
    
    /* 初始化套接字选项 */
    sock->so_reuseaddr = 0;
    sock->so_keepalive = 0;
    sock->so_broadcast = 0;
    sock->so_oobinline = 0;
    sock->so_error = 0;
    sock->so_acceptconn = 0;
    sock->tcp_nodelay = 0;
    
    /* TCP Keep-Alive 参数 (通过 setsockopt 配置) */
    sock->tcp_keepidle = 7200;     /* 空闲时间 (秒), 默认 2 小时 */
    sock->tcp_keepintvl = 75;      /* 探测间隔 (秒), 默认 75 秒 */
    sock->tcp_keepcnt = 9;         /* 最大探测次数, 默认 9 次 */
    
    sock->local_addrlen = 0;
    sock->remote_addrlen = 0;
    
    /* 分配缓冲区 */
    sock->recv_buf_size = 64 * 1024;  /* 64KB */
    sock->so_rcvbuf = sock->recv_buf_size;
    sock->recv_buf = kmalloc(sock->recv_buf_size);
    if (sock->recv_buf == NULL) {
        sock->ref_count = 0;  /* 标记为未使用 */
        return -1;  /* ENOMEM */
    }
    sock->recv_buf_len = 0;
    
    sock->send_buf_size = 64 * 1024;
    sock->so_sndbuf = sock->send_buf_size;
    sock->send_buf = kmalloc(sock->send_buf_size);
    if (sock->send_buf == NULL) {
        kfree(sock->recv_buf);  /* 释放已分配的接收缓冲区 */
        sock->ref_count = 0;
        return -1;  /* ENOMEM */
    }
    sock->send_buf_len = 0;
    
    sock->waiting_threads = 0;
    sock->ref_count = 1;
    sock->owns_recv_buf = 1;
    sock->unix_path[0] = '\0';
    sock->peer_fd = -1;
    
    print("[SOCKET] Created socket fd=");
    print_hex(fd);
    print(" domain=");
    print_hex(domain);
    print(" type=");
    print_hex(type);
    if (nonblock) print(" O_NONBLOCK");
    println("");
    
    return fd;
}

/* 绑定地址 */
int sys_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL) {
        return -1;  /* EBADF */
    }
    
    if (addrlen > sizeof(struct sockaddr_storage)) {
        addrlen = sizeof(struct sockaddr_storage);
    }
    
    /* Unix 域套接字: 绑定到文件系统路径 */
    if (sock->domain == AF_UNIX && addr != NULL) {
        struct sockaddr_un un_addr;
        if (addrlen > sizeof(struct sockaddr_un)) {
            addrlen = sizeof(struct sockaddr_un);
        }
        memcpy(&un_addr, addr, addrlen);
        
        /* 复制路径到套接字结构 */
        size_t path_len = addrlen - sizeof(uint16_t);  /* 减去 sun_family */
        if (path_len >= sizeof(sock->unix_path)) {
            path_len = sizeof(sock->unix_path) - 1;
        }
        memcpy(sock->unix_path, un_addr.sun_path, path_len);
        sock->unix_path[path_len] = '\0';
    }
    
    /* 复制地址 */
    for (size_t i = 0; i < addrlen; i++) {
        ((char *)&sock->local_addr)[i] = ((const char *)addr)[i];
    }
    sock->local_addrlen = addrlen;
    
    print("[SOCKET] Bound socket fd=");
    print_hex(sockfd);
    println("");
    
    return 0;
}

/* 监听连接 */
int sys_listen(int sockfd, int backlog) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL) {
        return -1;
    }
    
    /* 只有流套接字可以监听 */
    if (sock->type != SOCK_STREAM) {
        return -1;  /* EOPNOTSUPP */
    }
    
    sock->so_acceptconn = 1;
    (void)backlog;  /* 简化：忽略backlog */
    
    print("[SOCKET] Listening on socket fd=");
    print_hex(sockfd);
    println("");
    
    return 0;
}

/* 接受连接 */
int sys_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL) {
        return -1;
    }
    
    /* 创建新套接字用于连接 */
    int newfd = sys_socket(sock->domain, sock->type, sock->protocol);
    if (newfd < 0) {
        return newfd;
    }
    
    struct socket *newsock = get_socket(newfd);
    newsock->state = SS_CONNECTED;
    
    /* 复制远端地址 */
    if (addr != NULL && addrlen != NULL) {
        socklen_t len = sock->remote_addrlen;
        if (len > *addrlen) {
            len = *addrlen;
        }
        for (size_t i = 0; i < len; i++) {
            ((char *)addr)[i] = ((char *)&sock->remote_addr)[i];
        }
        *addrlen = sock->remote_addrlen;
    }
    
    print("[SOCKET] Accepted connection, new fd=");
    print_hex(newfd);
    println("");
    
    return newfd;
}

/* 发起连接 */
int sys_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL) {
        return -1;
    }
    
    if (addrlen > sizeof(struct sockaddr_storage)) {
        addrlen = sizeof(struct sockaddr_storage);
    }
    
    /* 复制远端地址 */
    for (size_t i = 0; i < addrlen; i++) {
        ((char *)&sock->remote_addr)[i] = ((const char *)addr)[i];
    }
    sock->remote_addrlen = addrlen;
    
    /* Unix 域套接字: 查找绑定到目标路径的对端 */
    if (sock->domain == AF_UNIX && addr != NULL) {
        struct sockaddr_un un_addr;
        if (addrlen > sizeof(struct sockaddr_un)) {
            addrlen = sizeof(struct sockaddr_un);
        }
        memcpy(&un_addr, addr, addrlen);
        
        /* 查找绑定到该路径的监听套接字 */
        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (socket_table[i].ref_count > 0 &&
                socket_table[i].domain == AF_UNIX &&
                socket_table[i].so_acceptconn &&
                socket_table[i].unix_path[0] != '\0') {
                /* 比较路径 */
                size_t path_len = addrlen - sizeof(uint16_t);
                if (path_len >= sizeof(un_addr.sun_path)) {
                    path_len = sizeof(un_addr.sun_path) - 1;
                }
                if (memcmp(socket_table[i].unix_path, un_addr.sun_path, path_len) == 0) {
                    /* 找到监听套接字，建立连接 */
                    sock->peer_fd = i;
                    sock->state = SS_CONNECTED;
                    
                    /* 共享缓冲区: 本端的 recv_buf 指向对端的 send_buf */
                    if (sock->owns_recv_buf && sock->recv_buf) {
                        kfree(sock->recv_buf);
                    }
                    sock->recv_buf = socket_table[i].send_buf;
                    sock->owns_recv_buf = 0;
                    
                    print("[SOCKET] Unix socket connected fd=");
                    print_hex(sockfd);
                    print(" -> peer=");
                    print_hex(i);
                    println("");
                    return 0;
                }
            }
        }
        
        /* 未找到监听套接字 */
        if (sock->nonblock) {
            return -EINPROGRESS;
        }
        return -1;  /* ECONNREFUSED */
    }
    
    /* 流套接字需要建立连接 */
    if (sock->type == SOCK_STREAM) {
        sock->state = SS_CONNECTED;
    }
    
    print("[SOCKET] Connected socket fd=");
    print_hex(sockfd);
    println("");
    
    return 0;
}

/* 发送数据 */
ssize_t sys_send(int sockfd, const void *buf, size_t len, int flags) {
    return sys_sendto(sockfd, buf, len, flags, NULL, 0);
}

ssize_t sys_sendto(int sockfd, const void *buf, size_t len, int flags,
                   const struct sockaddr *dest_addr, socklen_t addrlen) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL) {
        return -1;
    }
    
    (void)flags;
    (void)dest_addr;
    (void)addrlen;
    
    /* Unix 域套接字: 直接写入对端的接收缓冲区 */
    if (sock->domain == AF_UNIX && sock->peer_fd >= 0) {
        struct socket *peer = get_socket(sock->peer_fd);
        if (peer == NULL) {
            return -1;
        }
        
        size_t available = peer->recv_buf_size - peer->recv_buf_len;
        if (available == 0) {
            if (sock->nonblock) {
                return -EAGAIN;
            }
            return -1;
        }
        
        size_t to_send = len;
        if (to_send > available) {
            if (sock->nonblock) {
                to_send = available;
            } else {
                to_send = available;
            }
        }
        
        if (to_send > 0) {
            char *dst = (char *)peer->recv_buf + peer->recv_buf_len;
            memcpy(dst, buf, to_send);
            peer->recv_buf_len += to_send;
        }
        
        return to_send;
    }
    
    /* 简化实现：将数据存入发送缓冲区 */
    size_t available = sock->send_buf_size - sock->send_buf_len;
    
    if (available == 0) {
        /* 发送缓冲区满 */
        if (sock->nonblock) {
            return -EAGAIN;
        }
        /* 阻塞模式: 简化处理，返回错误 */
        return -1;
    }
    
    size_t to_send = len;
    if (to_send > available) {
        if (sock->nonblock) {
            /* 非阻塞模式: 发送能放下的部分 */
            to_send = available;
        } else {
            to_send = available;  /* 简化: 也只发送能放下的 */
        }
    }
    
    if (to_send > 0) {
        char *dst = (char *)sock->send_buf + sock->send_buf_len;
        const char *src = (const char *)buf;
        for (size_t i = 0; i < to_send; i++) {
            dst[i] = src[i];
        }
        sock->send_buf_len += to_send;
    }
    
    print("[SOCKET] Sent ");
    print_hex(to_send);
    print(" bytes on socket fd=");
    print_hex(sockfd);
    println("");
    
    return to_send;
}

/* 接收数据 */
ssize_t sys_recv(int sockfd, void *buf, size_t len, int flags) {
    return sys_recvfrom(sockfd, buf, len, flags, NULL, NULL);
}

ssize_t sys_recvfrom(int sockfd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL) {
        return -1;
    }
    
    (void)flags;
    (void)src_addr;
    (void)addrlen;
    
    /* 非阻塞模式: 无数据时立即返回 EAGAIN */
    if (sock->recv_buf_len == 0) {
        if (sock->nonblock) {
            return -EAGAIN;  /* EWOULDBLOCK */
        }
        /* 阻塞模式: 简化处理，返回 0 */
        return 0;
    }
    
    /* 从接收缓冲区读取 */
    size_t to_read = len;
    if (to_read > sock->recv_buf_len) {
        to_read = sock->recv_buf_len;
    }
    
    if (to_read > 0) {
        memcpy(buf, sock->recv_buf, to_read);
        
        /* 移动剩余数据 */
        sock->recv_buf_len -= to_read;
        if (sock->recv_buf_len > 0) {
            memmove(sock->recv_buf, (char *)sock->recv_buf + to_read, sock->recv_buf_len);
        }
    }
    
    print("[SOCKET] Received ");
    print_hex(to_read);
    print(" bytes on socket fd=");
    print_hex(sockfd);
    println("");
    
    return to_read;
}

/* 关闭连接 */
int sys_shutdown(int sockfd, int how) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL) {
        return -1;
    }
    
    switch (how) {
        case SHUT_RD:
            /* 关闭读 */
            sock->recv_buf_len = 0;
            break;
        case SHUT_WR:
            /* 关闭写 */
            sock->send_buf_len = 0;
            break;
        case SHUT_RDWR:
            /* 全关闭 */
            sock->state = SS_DISCONNECTING;
            break;
    }
    
    return 0;
}

/* 关闭套接字并释放资源 */
int sys_closesocket(int sockfd) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL) {
        return -1;
    }

    /* Only free recv_buf if we own it (not a socketpair alias) */
    if (sock->owns_recv_buf && sock->recv_buf != NULL) {
        kfree(sock->recv_buf);
    }
    if (sock->send_buf != NULL) {
        kfree(sock->send_buf);
    }
    sock->recv_buf = NULL;
    sock->send_buf = NULL;
    sock->ref_count = 0;
    sock->state = -1;

    return 0;
}

/* 获取/设置套接字选项 */
int sys_getsockopt(int sockfd, int level, int optname,
                   void *optval, socklen_t *optlen) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL || optval == NULL || optlen == NULL) {
        return -1;
    }
    
    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_REUSEADDR:
                *(int *)optval = sock->so_reuseaddr;
                *optlen = sizeof(int);
                return 0;
            case SO_KEEPALIVE:
                *(int *)optval = sock->so_keepalive;
                *optlen = sizeof(int);
                return 0;
            case SO_BROADCAST:
                *(int *)optval = sock->so_broadcast;
                *optlen = sizeof(int);
                return 0;
            case SO_RCVBUF:
                *(int *)optval = (int)sock->so_rcvbuf;
                *optlen = sizeof(int);
                return 0;
            case SO_SNDBUF:
                *(int *)optval = (int)sock->so_sndbuf;
                *optlen = sizeof(int);
                return 0;
            case SO_TYPE:
                *(int *)optval = sock->type;
                *optlen = sizeof(int);
                return 0;
            case SO_ERROR:
                *(int *)optval = sock->so_error;
                sock->so_error = 0;  /* 读取后清除错误 */
                *optlen = sizeof(int);
                return 0;
            case SO_ACCEPTCONN:
                *(int *)optval = sock->so_acceptconn;
                *optlen = sizeof(int);
                return 0;
            case SO_OOBINLINE:
                *(int *)optval = sock->so_oobinline;
                *optlen = sizeof(int);
                return 0;
            default:
                return -1;
        }
    } else if (level == SOL_TCP) {
        switch (optname) {
            case TCP_NODELAY:
                *(int *)optval = sock->tcp_nodelay;
                *optlen = sizeof(int);
                return 0;
            case TCP_KEEPIDLE:
                *(int *)optval = sock->tcp_keepidle;
                *optlen = sizeof(int);
                return 0;
            case TCP_KEEPINTVL:
                *(int *)optval = sock->tcp_keepintvl;
                *optlen = sizeof(int);
                return 0;
            case TCP_KEEPCNT:
                *(int *)optval = sock->tcp_keepcnt;
                *optlen = sizeof(int);
                return 0;
            default:
                return -1;
        }
    }
    
    return -1;
}

int sys_setsockopt(int sockfd, int level, int optname,
                   const void *optval, socklen_t optlen) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL || optval == NULL) {
        return -1;
    }
    
    if (optlen < sizeof(int)) {
        return -1;
    }
    
    int val = *(const int *)optval;
    
    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_REUSEADDR:
                sock->so_reuseaddr = val;
                return 0;
            case SO_KEEPALIVE:
                sock->so_keepalive = val;
                return 0;
            case SO_BROADCAST:
                sock->so_broadcast = val;
                return 0;
            case SO_RCVBUF: {
                /* 调整接收缓冲区大小 */
                size_t new_size = (size_t)val;
                if (new_size < 4096) new_size = 4096;
                if (new_size > 1024 * 1024) new_size = 1024 * 1024;
                void *new_buf = kmalloc(new_size);
                if (new_buf == NULL) return -1;
                /* 复制旧数据 */
                size_t copy_len = sock->recv_buf_len < new_size ? sock->recv_buf_len : new_size;
                if (copy_len > 0) {
                    memcpy(new_buf, sock->recv_buf, copy_len);
                }
                if (sock->owns_recv_buf && sock->recv_buf) {
                    kfree(sock->recv_buf);
                }
                sock->recv_buf = new_buf;
                sock->recv_buf_size = new_size;
                sock->so_rcvbuf = new_size;
                sock->owns_recv_buf = 1;
                return 0;
            }
            case SO_SNDBUF: {
                /* 调整发送缓冲区大小 */
                size_t new_size = (size_t)val;
                if (new_size < 4096) new_size = 4096;
                if (new_size > 1024 * 1024) new_size = 1024 * 1024;
                void *new_buf = kmalloc(new_size);
                if (new_buf == NULL) return -1;
                size_t copy_len = sock->send_buf_len < new_size ? sock->send_buf_len : new_size;
                if (copy_len > 0) {
                    memcpy(new_buf, sock->send_buf, copy_len);
                }
                if (sock->send_buf) {
                    kfree(sock->send_buf);
                }
                sock->send_buf = new_buf;
                sock->send_buf_size = new_size;
                sock->so_sndbuf = new_size;
                return 0;
            }
            case SO_OOBINLINE:
                sock->so_oobinline = val;
                return 0;
            default:
                return -1;
        }
    } else if (level == SOL_TCP) {
        switch (optname) {
            case TCP_NODELAY:
                sock->tcp_nodelay = val;
                return 0;
            case TCP_KEEPIDLE:
                /* Keep-Alive 空闲时间 (秒) */
                if (val < 1) val = 1;
                if (val > 32767) val = 32767;
                sock->tcp_keepidle = val;
                return 0;
            case TCP_KEEPINTVL:
                /* Keep-Alive 探测间隔 (秒) */
                if (val < 1) val = 1;
                if (val > 32767) val = 32767;
                sock->tcp_keepintvl = val;
                return 0;
            case TCP_KEEPCNT:
                /* Keep-Alive 最大探测次数 */
                if (val < 1) val = 1;
                if (val > 127) val = 127;
                sock->tcp_keepcnt = val;
                return 0;
            default:
                return -1;
        }
    }
    
    return -1;
}

/* 获取套接字地址 */
int sys_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL) {
        return -1;
    }
    
    socklen_t len = sock->local_addrlen;
    if (len > *addrlen) {
        len = *addrlen;
    }
    
    for (size_t i = 0; i < len; i++) {
        ((char *)addr)[i] = ((char *)&sock->local_addr)[i];
    }
    *addrlen = sock->local_addrlen;
    
    return 0;
}

int sys_getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    struct socket *sock = get_socket(sockfd);
    if (sock == NULL) {
        return -1;
    }
    
    socklen_t len = sock->remote_addrlen;
    if (len > *addrlen) {
        len = *addrlen;
    }
    
    for (size_t i = 0; i < len; i++) {
        ((char *)addr)[i] = ((char *)&sock->remote_addr)[i];
    }
    *addrlen = sock->remote_addrlen;
    
    return 0;
}

/* 套接字对 (用于进程间通信) */
int sys_socketpair(int domain, int type, int protocol, int sv[2]) {
    /* 只支持 Unix 域套接字 */
    if (domain != AF_UNIX) {
        return -1;  /* EAFNOSUPPORT */
    }
    
    /* 创建两个套接字 */
    sv[0] = sys_socket(domain, type, protocol);
    if (sv[0] < 0) {
        return -1;
    }
    
    sv[1] = sys_socket(domain, type, protocol);
    if (sv[1] < 0) {
        /* 关闭已创建的第一个套接字，避免描述符泄漏 */
        struct socket *sock0 = get_socket(sv[0]);
        if (sock0 != NULL) {
            void *recv_buf = sock0->recv_buf;
            void *send_buf = sock0->send_buf;
            sock0->recv_buf = NULL;
            sock0->send_buf = NULL;
            sock0->ref_count = 0;
            sock0->state = SS_UNCONNECTED;
            if (recv_buf != NULL) kfree(recv_buf);
            if (send_buf != NULL) kfree(send_buf);
        }
        sv[0] = -1;
        return -1;
    }
    
    /* 互连 */
    struct socket *sock0 = get_socket(sv[0]);
    struct socket *sock1 = get_socket(sv[1]);
    
    sock0->state = SS_CONNECTED;
    sock1->state = SS_CONNECTED;
    
    /* 共享缓冲区: recv_buf aliases peer's send_buf, not owned */
    sock0->recv_buf = sock1->send_buf;
    sock0->owns_recv_buf = 0;
    sock1->recv_buf = sock0->send_buf;
    sock1->owns_recv_buf = 0;
    
    /* 设置对端 fd (Unix 域套接字) */
    sock0->peer_fd = sv[1];
    sock1->peer_fd = sv[0];
    
    print("[SOCKET] Created socket pair [");
    print_hex(sv[0]);
    print(", ");
    print_hex(sv[1]);
    println("]");
    
    return 0;
}

/* poll 实现 (增强版: 支持超时、Unix 域套接字、更多事件) */
int sys_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    int ready = 0;
    uint64_t start_time = 0;
    
    if (timeout > 0) {
        start_time = get_tick_count();
    }
    
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        
        if (fds[i].fd < 0) {
            fds[i].fd = -1;
            continue;
        }
        
        struct socket *sock = get_socket(fds[i].fd);
        if (sock == NULL) {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        
        /* 检查可读 */
        if (fds[i].events & POLLIN) {
            /* Unix 域套接字: 检查对端发送缓冲区 */
            if (sock->domain == AF_UNIX && sock->peer_fd >= 0) {
                struct socket *peer = get_socket(sock->peer_fd);
                if (peer != NULL && peer->send_buf_len > 0) {
                    /* 对端有数据发来 (本端的 recv_buf 即对端的 send_buf) */
                    fds[i].revents |= POLLIN;
                }
            }
            
            if (sock->recv_buf_len > 0) {
                fds[i].revents |= POLLIN;
            } else if (sock->state == SS_DISCONNECTING) {
                fds[i].revents |= POLLHUP;
            } else if (sock->so_acceptconn) {
                /* 监听套接字: 检查是否有待处理的连接 */
                fds[i].revents |= POLLIN;
            }
        }
        
        /* 检查可写 */
        if (fds[i].events & POLLOUT) {
            /* Unix 域套接字: 检查对端接收缓冲区是否有空间 */
            if (sock->domain == AF_UNIX && sock->peer_fd >= 0) {
                struct socket *peer = get_socket(sock->peer_fd);
                if (peer != NULL && peer->recv_buf_len < peer->recv_buf_size) {
                    fds[i].revents |= POLLOUT;
                }
            } else {
                if (sock->send_buf_len < sock->send_buf_size) {
                    fds[i].revents |= POLLOUT;
                }
            }
            
            /* 正在连接的非阻塞套接字 */
            if (sock->state == SS_CONNECTING) {
                fds[i].revents |= POLLOUT;
            }
        }
        
        /* 检查错误 */
        if ((fds[i].events & POLLERR) || sock->so_error) {
            if (sock->so_error) {
                fds[i].revents |= POLLERR;
            }
        }
        
        /* 检查挂起 */
        if (sock->state == SS_DISCONNECTING) {
            fds[i].revents |= POLLHUP;
        }
        
        if (fds[i].revents != 0) {
            ready++;
        }
    }
    
    /* 超时等待 (轮询等待，带退避) */
    if (ready == 0 && timeout != 0) {
        uint64_t deadline = start_time + (uint64_t)timeout;
        int poll_count = 0;
        
        while (ready == 0) {
            /* 检查超时 (timeout < 0 表示无限等待) */
            if (timeout > 0 && get_tick_count() >= deadline) {
                break;
            }
            
            /* 重新检查所有 fd */
            for (nfds_t i = 0; i < nfds; i++) {
                if (fds[i].fd < 0) continue;
                
                struct socket *sock = get_socket(fds[i].fd);
                if (sock == NULL) continue;
                
                if ((fds[i].events & POLLIN) && sock->recv_buf_len > 0) {
                    fds[i].revents |= POLLIN;
                }
                if ((fds[i].events & POLLOUT) && sock->send_buf_len < sock->send_buf_size) {
                    fds[i].revents |= POLLOUT;
                }
                if (sock->so_error) {
                    fds[i].revents |= POLLERR;
                }
                if (sock->state == SS_DISCONNECTING) {
                    fds[i].revents |= POLLHUP;
                }
                
                if (fds[i].revents != 0) ready++;
            }
            
            if (ready == 0) {
                /* 退避: 逐步增加等待间隔 */
                poll_count++;
                if (poll_count < 10) {
                    for (volatile int j = 0; j < 100; j++);  /* 短暂忙等 */
                } else if (poll_count < 100) {
                    asm volatile("pause");
                } else {
                    /* 让出 CPU (简化: 使用 pause 指令) */
                    for (volatile int j = 0; j < 1000; j++);
                    asm volatile("pause");
                }
            }
        }
    }
    
    return ready;
}

/* select 实现 */
int sys_select(int nfds, fd_set *readfds, fd_set *writefds, 
               fd_set *exceptfds, struct timeval *timeout) {
    int ready = 0;
    uint64_t deadline = 0;
    
    if (timeout && (timeout->tv_sec > 0 || timeout->tv_usec > 0)) {
        deadline = get_tick_count() + timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
    }
    
    /* 转换为 poll 格式 */
    struct pollfd pfds[FD_SETSIZE];
    nfds_t pfd_count = 0;
    
    for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
        short events = 0;
        
        if (readfds && FD_ISSET(fd, readfds)) {
            events |= POLLIN;
        }
        if (writefds && FD_ISSET(fd, writefds)) {
            events |= POLLOUT;
        }
        if (exceptfds && FD_ISSET(fd, exceptfds)) {
            events |= POLLERR;
        }
        
        if (events) {
            pfds[pfd_count].fd = fd;
            pfds[pfd_count].events = events;
            pfds[pfd_count].revents = 0;
            pfd_count++;
        }
    }
    
    /* 计算超时 */
    int poll_timeout = 0;
    if (timeout) {
        if (timeout->tv_sec == 0 && timeout->tv_usec == 0) {
            poll_timeout = 0;  /* 非阻塞 */
        } else if (deadline > 0) {
            uint64_t now = get_tick_count();
            if (now >= deadline) {
                poll_timeout = 0;
            } else {
                poll_timeout = (int)(deadline - now);
            }
        }
    } else {
        poll_timeout = -1;  /* 无限等待 (简化为 0) */
        poll_timeout = 0;
    }
    
    /* 调用 poll */
    int ret = sys_poll(pfds, pfd_count, poll_timeout);
    if (ret < 0) return -1;
    
    /* 清除原始 fd_set 并设置返回的 fd_set */
    if (readfds) {
        for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
            FD_CLR(fd, readfds);
        }
    }
    if (writefds) {
        for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
            FD_CLR(fd, writefds);
        }
    }
    if (exceptfds) {
        for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
            FD_CLR(fd, exceptfds);
        }
    }
    
    /* 从 poll 结果设置 fd_set */
    for (nfds_t i = 0; i < pfd_count; i++) {
        int fd = pfds[i].fd;
        
        if (pfds[i].revents & POLLIN) {
            if (readfds) FD_SET(fd, readfds);
            ready++;
        }
        if (pfds[i].revents & POLLOUT) {
            if (writefds) FD_SET(fd, writefds);
            ready++;
        }
        if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (exceptfds) FD_SET(fd, exceptfds);
            ready++;
        }
    }
    
    return ready;
}
