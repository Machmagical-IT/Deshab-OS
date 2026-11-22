/*
 * Zratro OS - Error Number Definitions
 * 
 * 统一的内核错误码定义
 * 兼容 POSIX 标准
 */

#ifndef ERRNO_H
#define ERRNO_H

/* ========== 基本错误码 (1-34) ========== */
#define EPERM           1       /* Operation not permitted */
#define ENOENT          2       /* No such file or directory */
#define ESRCH           3       /* No such process */
#define EINTR           4       /* Interrupted system call */
#define EIO             5       /* I/O error */
#define ENXIO           6       /* No such device or address */
#define E2BIG           7       /* Argument list too long */
#define ENOEXEC         8       /* Exec format error */
#define EBADF           9       /* Bad file number */
#define ECHILD          10      /* No child processes */
#define EAGAIN          11      /* Try again */
#define ENOMEM          12      /* Out of memory */
#define EACCES          13      /* Permission denied */
#define EFAULT          14      /* Bad address */
#define ENOTBLK         15      /* Block device required */
#define EBUSY           16      /* Device or resource busy */
#define EEXIST          17      /* File exists */
#define EXDEV           18      /* Cross-device link */
#define ENODEV          19      /* No such device */
#define ENOTDIR         20      /* Not a directory */
#define EISDIR          21      /* Is a directory */
#define EINVAL          22      /* Invalid argument */
#define ENFILE          23      /* File table overflow */
#define EMFILE          24      /* Too many open files */
#define ENOTTY          25      /* Not a typewriter */
#define ETXTBSY         26      /* Text file busy */
#define EFBIG           27      /* File too large */
#define ENOSPC          28      /* No space left on device */
#define ESPIPE          29      /* Illegal seek */
#define EROFS           30      /* Read-only file system */
#define EMLINK          31      /* Too many links */
#define EPIPE           32      /* Broken pipe */
#define EDOM            33      /* Math argument out of domain */
#define ERANGE          34      /* Math result not representable */

/* ========== 扩展错误码 (35-87) ========== */
#define EDEADLK         35      /* Resource deadlock would occur */
#define ENAMETOOLONG    36      /* File name too long */
#define ENOLCK          37      /* No record locks available */
#define ENOSYS          38      /* Function not implemented */
#define ENOTEMPTY       39      /* Directory not empty */
#define ELOOP           40      /* Too many symbolic links */
#define ENOMSG          42      /* No message of desired type */
#define EIDRM           43      /* Identifier removed */
#define ECHRNG          44      /* Channel number out of range */
#define EL2NSYNC        45      /* Level 2 not synchronized */
#define EL3HLT          46      /* Level 3 halted */
#define EL3RST          47      /* Level 3 reset */
#define ELNRNG          48      /* Link number out of range */
#define EUNATCH         49      /* Protocol driver not attached */
#define ENOCSI          50      /* No CSI structure available */
#define EL2HLT          51      /* Level 2 halted */
#define EBADE           52      /* Invalid exchange */
#define EBADR           53      /* Invalid request descriptor */
#define EXFULL          54      /* Exchange full */
#define ENOANO          55      /* No anode */
#define EBADRQC         56      /* Invalid request code */
#define EBADSLT         57      /* Invalid slot */
#define EDEADLOCK       EDEADLK /* File locking deadlock error */
#define EBFONT          59      /* Bad font file format */
#define ENOSTR          60      /* Device not a stream */
#define ENODATA         61      /* No data available */
#define ETIME           62      /* Timer expired */
#define ENOSR           63      /* Out of streams resources */
#define ENONET          64      /* Machine is not on the network */
#define ENOPKG          65      /* Package not installed */
#define EREMOTE         66      /* Object is remote */
#define ENOLINK         67      /* Link has been severed */
#define EADV            68      /* Advertise error */
#define ESRMNT          69      /* Srmount error */
#define ECOMM           70      /* Communication error on send */
#define EPROTO          71      /* Protocol error */
#define EMULTIHOP       72      /* Multihop attempted */
#define EDOTDOT         73      /* RFS specific error */
#define EBADMSG         74      /* Not a data message */
#define EOVERFLOW       75      /* Value too large for defined data type */
#define ENOTUNIQ        76      /* Name not unique on network */
#define EBADFD          77      /* File descriptor in bad state */
#define EREMCHG         78      /* Remote address changed */
#define ELIBACC         79      /* Can not access a needed shared library */
#define ELIBBAD         80      /* Accessing a corrupted shared library */
#define ELIBSCN         81      /* .lib section in a.out corrupted */
#define ELIBMAX         82      /* Attempting to link in too many shared libraries */
#define ELIBEXEC        83      /* Cannot exec a shared library directly */
#define EILSEQ          84      /* Illegal byte sequence */
#define ERESTART        85      /* Interrupted system call should be restarted */
#define ESTRPIPE        86      /* Streams pipe error */
#define EUSERS          87      /* Too many users */

/* ========== 网络相关错误码 (88-131) ========== */
#define ENOTSOCK        88      /* Socket operation on non-socket */
#define EDESTADDRREQ    89      /* Destination address required */
#define EMSGSIZE        90      /* Message too long */
#define EPROTOTYPE      91      /* Protocol wrong type for socket */
#define ENOPROTOOPT     92      /* Protocol not available */
#define EPROTONOSUPPORT 93      /* Protocol not supported */
#define ESOCKTNOSUPPORT 94      /* Socket type not supported */
#define EOPNOTSUPP      95      /* Operation not supported on transport endpoint */
#define EPFNOSUPPORT    96      /* Protocol family not supported */
#define EAFNOSUPPORT    97      /* Address family not supported by protocol */
#define EADDRINUSE      98      /* Address already in use */
#define EADDRNOTAVAIL   99      /* Cannot assign requested address */
#define ENETDOWN        100     /* Network is down */
#define ENETUNREACH     101     /* Network is unreachable */
#define ENETRESET       102     /* Network dropped connection because of reset */
#define ECONNABORTED    103     /* Software caused connection abort */
#define ECONNRESET      104     /* Connection reset by peer */
#define ENOBUFS         105     /* No buffer space available */
#define EISCONN         106     /* Transport endpoint is already connected */
#define ENOTCONN        107     /* Transport endpoint is not connected */
#define ESHUTDOWN       108     /* Cannot send after transport endpoint shutdown */
#define ETOOMANYREFS    109     /* Too many references: cannot splice */
#define ETIMEDOUT       110     /* Connection timed out */
#define ECONNREFUSED    111     /* Connection refused */
#define EHOSTDOWN       112     /* Host is down */
#define EHOSTUNREACH    113     /* No route to host */
#define EALREADY        114     /* Operation already in progress */
#define EINPROGRESS     115     /* Operation now in progress */
#define ESTALE          116     /* Stale NFS file handle */
#define EUCLEAN         117     /* Structure needs cleaning */
#define ENOTNAM         118     /* Not a XENIX named type file */
#define ENAVAIL         119     /* No XENIX semaphores available */
#define EISNAM          120     /* Is a named type file */
#define EREMOTEIO       121     /* Remote I/O error */
#define EDQUOT          122     /* Quota exceeded */
#define ENOMEDIUM       123     /* No medium found */
#define EMEDIUMTYPE     124     /* Wrong medium type */
#define ECANCELED       125     /* Operation Canceled */
#define ENOKEY          126     /* Required key not available */
#define EKEYEXPIRED     127     /* Key has expired */
#define EKEYREVOKED     128     /* Key has been revoked */
#define EKEYREJECTED    129     /* Key was rejected by service */
#define EOWNERDEAD      130     /* Owner died */
#define ENOTRECOVERABLE 131     /* State not recoverable */

/* ========== 内核内部错误码 (132+) ========== */
#define EKRNLFS         132     /* Kernel filesystem error */
#define EKRNLIRQ        133     /* Kernel IRQ error */
#define EKRNLMM         134     /* Kernel memory management error */
#define EKRNLPRC        135     /* Kernel process error */
#define EKRNLDEV        136     /* Kernel device error */
#define EKRNLTIMEOUT    137     /* Kernel timeout error */
#define EKRNLLOCK       138     /* Kernel lock error */
#define EKRNLBUF        139     /* Kernel buffer error */

/* ========== 辅助宏 ========== */

/* 错误码转负数 (用于返回值) */
#define ERR(err)        (-(int)(err))
#define IS_ERR_VALUE(x) ((unsigned long)(void *)(x) >= (unsigned long)-4096)

/* 从指针判断错误 */
#define IS_ERR(ptr)     IS_ERR_VALUE((unsigned long)(ptr))
#define ERR_PTR(err)    ((void *)((unsigned long)(err)))
#define PTR_ERR(ptr)    ((unsigned long)(ptr))

/* 安全返回宏 */
#define RET_ERR(err)    do { return ERR(err); } while(0)
#define RET_NULL        do { return NULL; } while(0)
#define RET_OK          do { return 0; } while(0)

/* 检查并返回错误 */
#define CHECK_PTR(ptr)  do { if ((ptr) == NULL) RET_ERR(ENOMEM); } while(0)
#define CHECK_PTR_GOTO(ptr, label) do { if ((ptr) == NULL) goto label; } while(0)

/* ========== 错误消息 ========== */

static inline const char *strerror_kernel(int errnum) {
    switch (errnum) {
        case EPERM:     return "Operation not permitted";
        case ENOENT:    return "No such file or directory";
        case ESRCH:     return "No such process";
        case EINTR:     return "Interrupted system call";
        case EIO:       return "I/O error";
        case ENXIO:     return "No such device or address";
        case E2BIG:     return "Argument list too long";
        case ENOEXEC:   return "Exec format error";
        case EBADF:     return "Bad file descriptor";
        case ECHILD:    return "No child processes";
        case EAGAIN:    return "Resource temporarily unavailable";
        case ENOMEM:    return "Cannot allocate memory";
        case EACCES:    return "Permission denied";
        case EFAULT:    return "Bad address";
        case EBUSY:     return "Device or resource busy";
        case EEXIST:    return "File exists";
        case ENODEV:    return "No such device";
        case ENOTDIR:   return "Not a directory";
        case EISDIR:    return "Is a directory";
        case EINVAL:    return "Invalid argument";
        case ENFILE:    return "File table overflow";
        case EMFILE:    return "Too many open files";
        case ENOSPC:    return "No space left on device";
        case EPIPE:     return "Broken pipe";
        case EDEADLK:   return "Resource deadlock avoided";
        case ENAMETOOLONG: return "File name too long";
        case ENOSYS:    return "Function not implemented";
        case ENOTEMPTY: return "Directory not empty";
        case ELOOP:     return "Too many symbolic links";
        case ENOTSOCK:  return "Socket operation on non-socket";
        case EADDRINUSE:return "Address already in use";
        case EADDRNOTAVAIL: return "Cannot assign requested address";
        case ENETDOWN:  return "Network is down";
        case ENETUNREACH: return "Network is unreachable";
        case ECONNRESET: return "Connection reset by peer";
        case ETIMEDOUT: return "Connection timed out";
        case ECONNREFUSED: return "Connection refused";
        default:        return "Unknown error";
    }
}

#endif /* ERRNO_H */
