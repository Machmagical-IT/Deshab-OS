/*
 * Zratro OS - Signal Handling Subsystem Implementation
 */

#include "signal.h"
#include "task.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"

/* 信号名称表 */
static const char *signal_names[NSIG] = {
    [0] = "NULL",
    [SIGHUP] = "SIGHUP",
    [SIGINT] = "SIGINT",
    [SIGQUIT] = "SIGQUIT",
    [SIGILL] = "SIGILL",
    [SIGTRAP] = "SIGTRAP",
    [SIGABRT] = "SIGABRT",
    [SIGBUS] = "SIGBUS",
    [SIGFPE] = "SIGFPE",
    [SIGKILL] = "SIGKILL",
    [SIGUSR1] = "SIGUSR1",
    [SIGSEGV] = "SIGSEGV",
    [SIGUSR2] = "SIGUSR2",
    [SIGPIPE] = "SIGPIPE",
    [SIGALRM] = "SIGALRM",
    [SIGTERM] = "SIGTERM",
    [SIGSTKFLT] = "SIGSTKFLT",
    [SIGCHLD] = "SIGCHLD",
    [SIGCONT] = "SIGCONT",
    [SIGSTOP] = "SIGSTOP",
    [SIGTSTP] = "SIGTSTP",
    [SIGTTIN] = "SIGTTIN",
    [SIGTTOU] = "SIGTTOU",
    [SIGURG] = "SIGURG",
    [SIGXCPU] = "SIGXCPU",
    [SIGXFSZ] = "SIGXFSZ",
    [SIGVTALRM] = "SIGVTALRM",
    [SIGPROF] = "SIGPROF",
    [SIGWINCH] = "SIGWINCH",
    [SIGIO] = "SIGIO",
    [SIGPWR] = "SIGPWR",
    [SIGSYS] = "SIGSYS",
};

/* 默认信号行为 */
#define SIG_ACT_TERM    0   /* 终止进程 */
#define SIG_ACT_CORE    1   /* 终止并产生 core dump */
#define SIG_ACT_IGN     2   /* 忽略 */
#define SIG_ACT_STOP    3   /* 停止进程 */
#define SIG_ACT_CONT    4   /* 继续进程 */

/* 实时信号标志 */
#define SIG_QUEUE_LIMIT         0x01   /* 队列限制检查 */

static const int default_actions[NSIG] = {
    [0] = SIG_ACT_IGN,
    [SIGHUP] = SIG_ACT_TERM,
    [SIGINT] = SIG_ACT_TERM,
    [SIGQUIT] = SIG_ACT_CORE,
    [SIGILL] = SIG_ACT_CORE,
    [SIGTRAP] = SIG_ACT_CORE,
    [SIGABRT] = SIG_ACT_CORE,
    [SIGBUS] = SIG_ACT_CORE,
    [SIGFPE] = SIG_ACT_CORE,
    [SIGKILL] = SIG_ACT_TERM,
    [SIGUSR1] = SIG_ACT_TERM,
    [SIGSEGV] = SIG_ACT_CORE,
    [SIGUSR2] = SIG_ACT_TERM,
    [SIGPIPE] = SIG_ACT_TERM,
    [SIGALRM] = SIG_ACT_TERM,
    [SIGTERM] = SIG_ACT_TERM,
    [SIGSTKFLT] = SIG_ACT_TERM,
    [SIGCHLD] = SIG_ACT_IGN,
    [SIGCONT] = SIG_ACT_CONT,
    [SIGSTOP] = SIG_ACT_STOP,
    [SIGTSTP] = SIG_ACT_STOP,
    [SIGTTIN] = SIG_ACT_STOP,
    [SIGTTOU] = SIG_ACT_STOP,
    [SIGURG] = SIG_ACT_IGN,
    [SIGXCPU] = SIG_ACT_CORE,
    [SIGXFSZ] = SIG_ACT_CORE,
    [SIGVTALRM] = SIG_ACT_TERM,
    [SIGPROF] = SIG_ACT_TERM,
    [SIGWINCH] = SIG_ACT_IGN,
    [SIGIO] = SIG_ACT_TERM,
    [SIGPWR] = SIG_ACT_TERM,
    [SIGSYS] = SIG_ACT_CORE,
};

/* 初始化信号子系统 */
int signal_init(void) {
    println("[SIGNAL] Initializing signal subsystem...");
    println("[SIGNAL] Initialized successfully");
    return 0;
}

/* 初始化进程信号状态 */
int signal_state_init(struct signal_state *state) {
    if (state == NULL) return -1;
    
    memset(state, 0, sizeof(*state));
    
    /* 初始化所有信号动作为默认 */
    for (int i = 1; i < NSIG; i++) {
        state->actions[i].sa_handler = SIG_DFL;
        sigemptyset(&state->actions[i].sa_mask);
        state->actions[i].sa_flags = 0;
    }
    
    /* 初始化信号掩码 */
    sigemptyset(&state->blocked);
    sigemptyset(&state->pending);
    
    /* 初始化备用栈 */
    state->altstack.ss_sp = NULL;
    state->altstack.ss_size = 0;
    state->altstack.ss_flags = SS_DISABLE;
    
    state->queue_head = NULL;
    state->queue_tail = NULL;
    state->has_pending = 0;
    
    return 0;
}

/* 销毁进程信号状态 */
void signal_state_destroy(struct signal_state *state) {
    if (state == NULL) return;
    
    /* 释放信号队列 */
    struct sigqueue *q = state->queue_head;
    while (q != NULL) {
        struct sigqueue *next = q->next;
        kfree(q);
        q = next;
    }
    
    state->queue_head = NULL;
    state->queue_tail = NULL;
}

/* 检查信号是否可捕获 */
int signal_is_catchable(int sig) {
    return (sig > 0 && sig < NSIG && sig != SIGKILL && sig != SIGSTOP);
}

/* 获取默认信号动作 */
int signal_get_default_action(int sig) {
    if (sig <= 0 || sig >= NSIG) return SIG_ACT_TERM;
    return default_actions[sig];
}

/* 信号名称 */
const char *signal_name(int sig) {
    if (sig >= 0 && sig < NSIG) {
        return signal_names[sig] ? signal_names[sig] : "UNKNOWN";
    }
    return "INVALID";
}

/* 排队信号 */
static int signal_queue(struct signal_state *state, int sig, siginfo_t *info) {
    /* 检查队列长度限制 */
    if (state->queue_count >= SIGNAL_QUEUE_MAX) {
        print("[SIGNAL] Queue full, dropping signal ");
        print_dec(sig);
        println("");
        return -1;  /* EAGAIN */
    }
    
    struct sigqueue *q = kmalloc(sizeof(*q));
    if (q == NULL) return -1;
    
    q->next = NULL;
    if (info) {
        q->info = *info;
    } else {
        memset(&q->info, 0, sizeof(q->info));
        q->info.si_signo = sig;
        q->info.si_code = SI_KERNEL;
    }
    
    if (state->queue_tail) {
        state->queue_tail->next = q;
    } else {
        state->queue_head = q;
    }
    state->queue_tail = q;
    state->queue_count++;
    
    return 0;
}

/* 发送信号给进程 */
int signal_send(pid_t pid, int sig) {
    return signal_send_info(pid, sig, NULL);
}

int signal_send_info(pid_t pid, int sig, siginfo_t *info) {
    if (sig < 0 || sig >= NSIG) return -1;
    
    struct process *proc = process_get_by_pid(pid);
    if (proc == NULL) return -1;
    
    /* 检查进程是否有信号状态 */
    if (proc->signals == NULL) {
        return -1;
    }
    
    /* SIGKILL 和 SIGSTOP 总是处理 (不可被阻塞) */
    if (sig == SIGKILL || sig == SIGSTOP) {
        goto deliver;
    }

    /* SIGCONT 可唤醒停止的进程 */
    if (sig == SIGCONT) {
        /* 清除待处理的停止信号 */
        sigdelset(&proc->signals->pending, SIGSTOP);
        sigdelset(&proc->signals->pending, SIGTSTP);
        sigdelset(&proc->signals->pending, SIGTTIN);
        sigdelset(&proc->signals->pending, SIGTTOU);
        goto deliver;
    }
    
    /* 获取信号状态锁 */
    spin_lock(&proc->signals->lock);
    
    /* 检查信号是否被阻塞 */
    if (sigismember(&proc->signals->blocked, sig)) {
        /* 加入待处理集 */
        sigaddset(&proc->signals->pending, sig);
        proc->signals->has_pending = 1;
        
        /* 标准信号不排队，只记录一次 */
        if (sig < SIGRTMIN) {
            spin_unlock(&proc->signals->lock);
            return 0;
        }
        
        /* 实时信号排队 */
        int ret = signal_queue(proc->signals, sig, info);
        spin_unlock(&proc->signals->lock);
        return ret;
    }
    
    /* 检查信号动作 */
    struct sigaction *act = &proc->signals->actions[sig];
    
    if (act->sa_handler == SIG_IGN && sig != SIGCHLD) {
        /* 忽略信号 */
        spin_unlock(&proc->signals->lock);
        return 0;
    }
    
    spin_unlock(&proc->signals->lock);
    
deliver:
    /* 检查信号动作 (已释放锁) */
    struct sigaction act_copy;
    spin_lock(&proc->signals->lock);
    act_copy = proc->signals->actions[sig];
    spin_unlock(&proc->signals->lock);
    
    if (act_copy.sa_handler == SIG_IGN && sig != SIGCHLD) {
        return 0;
    }
    
    if (act_copy.sa_handler == SIG_DFL) {
        /* 默认动作 */
        int action = default_actions[sig];
        
        switch (action) {
            case SIG_ACT_IGN:
                return 0;
                
            case SIG_ACT_TERM:
                process_exit(128 + sig);
                return 0;
                
            case SIG_ACT_CORE:
                /* TODO: 生成 core dump */
                process_exit(128 + sig);
                return 0;
                
            case SIG_ACT_STOP:
                /* SIGSTOP: 停止进程，将线程从运行队列移除 */
                {
                    struct thread *t = proc->threads;
                    while (t != NULL) {
                        t->flags |= THREAD_FLAG_STOPPED;
                        if (t->state == THREAD_STATE_READY && t->cpu >= 0) {
                            extern void scheduler_remove_thread(struct thread *th);
                            scheduler_remove_thread(t);
                        }
                        if (t->state == THREAD_STATE_RUNNING) {
                            t->state = THREAD_STATE_BLOCKED;
                        }
                        t = t->process_next;
                    }
                }
                return 0;
                
            case SIG_ACT_CONT:
                /* SIGCONT: 继续进程，将线程重新加入运行队列 */
                {
                    struct thread *t = proc->threads;
                    while (t != NULL) {
                        if (t->flags & THREAD_FLAG_STOPPED) {
                            t->flags &= ~THREAD_FLAG_STOPPED;
                            if (t->state == THREAD_STATE_BLOCKED) {
                                t->state = THREAD_STATE_READY;
                                extern void scheduler_add_thread(struct thread *th);
                                scheduler_add_thread(t);
                            }
                        }
                        t = t->process_next;
                    }
                }
                return 0;
        }
    }
    
    /* 用户定义的处理函数 - 建立信号帧 */
    struct thread *thread = proc->threads;
    while (thread) {
        thread->flags |= THREAD_FLAG_SIGPENDING;
        thread = thread->process_next;
    }
    
    /* 排队信号 */
    spin_lock(&proc->signals->lock);
    sigaddset(&proc->signals->pending, sig);
    proc->signals->has_pending = 1;
    int ret = signal_queue(proc->signals, sig, info);
    spin_unlock(&proc->signals->lock);
    
    return ret;
}

/* 发送信号给线程 */
int signal_send_thread(uint32_t tid, int sig) {
    if (sig < 0 || sig >= NSIG) return -1;
    
    struct thread *thread = thread_get_by_tid(tid);
    if (thread == NULL) return -1;
    
    /* 标记线程有待处理信号 */
    thread->flags |= THREAD_FLAG_SIGPENDING;
    
    /* 发送给进程 */
    return signal_send(thread->pid, sig);
}

/* 发送信号给进程组 */
int signal_send_group(int32_t pgid, int sig) {
    if (sig < 0 || sig >= NSIG) return -1;
    
    int count = 0;
    
    /* 遍历所有进程 */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        struct process *proc = process_get_by_pid(i + 1);
        if (proc && proc->parent_pid == pgid) {
            signal_send(proc->pid, sig);
            count++;
        }
    }
    
    return count;
}

/* 处理待处理信号 */
int signal_handle_pending(struct signal_state *state, struct cpu_context *context) {
    if (state == NULL || context == NULL) return -1;
    
    /* 找出最高优先级的待处理信号 */
    for (int sig = 1; sig < NSIG; sig++) {
        if (!sigismember(&state->pending, sig)) continue;
        if (sigismember(&state->blocked, sig)) continue;
        
        /* 从待处理集中移除 */
        sigdelset(&state->pending, sig);
        
        /* 从队列中取出信号信息 */
        siginfo_t info = {0};
        info.si_signo = sig;
        info.si_code = SI_KERNEL;
        
        struct sigqueue *q = state->queue_head;
        if (q) {
            info = q->info;
            state->queue_head = q->next;
            state->queue_count--;
            if (state->queue_head == NULL) {
                state->queue_tail = NULL;
            }
            kfree(q);
        }
        
        /* 执行信号处理 */
        struct sigaction *act = &state->actions[sig];
        
        if (act->sa_handler == SIG_IGN) {
            continue;
        }
        
        if (act->sa_handler == SIG_DFL) {
            int action = default_actions[sig];
            
            switch (action) {
                case SIG_ACT_IGN:
                    continue;
                case SIG_ACT_TERM:
                case SIG_ACT_CORE:
                    process_exit(128 + sig);
                    return 0;
                case SIG_ACT_STOP:
                    /* 停止当前进程: 设置 STOPPED 标志，从运行队列移除 */
                    {
                        struct process *cur_proc = process_get_current();
                        if (cur_proc != NULL) {
                            struct thread *t = cur_proc->threads;
                            while (t != NULL) {
                                t->flags |= THREAD_FLAG_STOPPED;
                                if (t->state == THREAD_STATE_READY && t->cpu >= 0) {
                                    extern void scheduler_remove_thread(struct thread *th);
                                    scheduler_remove_thread(t);
                                }
                                if (t->state == THREAD_STATE_RUNNING) {
                                    t->state = THREAD_STATE_BLOCKED;
                                }
                                t = t->process_next;
                            }
                        }
                    }
                    continue;
                case SIG_ACT_CONT:
                    /* 继续当前进程: 清除 STOPPED 标志，加入运行队列 */
                    {
                        struct process *cur_proc = process_get_current();
                        if (cur_proc != NULL) {
                            struct thread *t = cur_proc->threads;
                            while (t != NULL) {
                                if (t->flags & THREAD_FLAG_STOPPED) {
                                    t->flags &= ~THREAD_FLAG_STOPPED;
                                    if (t->state == THREAD_STATE_BLOCKED) {
                                        t->state = THREAD_STATE_READY;
                                        extern void scheduler_add_thread(struct thread *th);
                                        scheduler_add_thread(t);
                                    }
                                }
                                t = t->process_next;
                            }
                        }
                    }
                    continue;
            }
        }
        
        /* 建立信号帧 */
        struct signal_frame frame;
        frame.sig = sig;
        frame.info = info;
        frame.action = act;
        frame.context = *context;
        frame.onstack = 0;
        
        /* 检查是否使用备用栈 */
        if (act->sa_flags & SA_ONSTACK && 
            state->altstack.ss_flags != SS_DISABLE &&
            !(state->altstack.ss_flags & SS_ONSTACK)) {
            frame.onstack = 1;
            state->altstack.ss_flags |= SS_ONSTACK;
        }
        
        /* 阻塞信号 */
        if (!(act->sa_flags & SA_NODEFER)) {
            sigaddset(&state->blocked, sig);
        }
        
        /* 阻塞 sa_mask 中的信号 */
        for (int i = 1; i < NSIG; i++) {
            if (sigismember(&act->sa_mask, i)) {
                sigaddset(&state->blocked, i);
            }
        }
        
        /* 修改上下文以调用信号处理函数 */
        if (act->sa_flags & SA_SIGINFO) {
            /* 三参数版本 */
            context->rdi = sig;          /* 第一个参数: 信号编号 */
            context->rsi = (uint64_t)&info; /* 第二个参数: siginfo_t* */
            context->rdx = 0;            /* 第三个参数: void* (ucontext) */
            context->rip = (uint64_t)act->sa_sigaction;
        } else {
            /* 单参数版本 */
            context->rdi = sig;
            context->rip = (uint64_t)act->sa_handler;
        }
        
        /* 设置栈 (使用用户栈或备用栈) */
        uint64_t sp;
        if (frame.onstack) {
            sp = (uint64_t)state->altstack.ss_sp + state->altstack.ss_size;
        } else {
            sp = context->rsp;
        }
        
        /* 在栈上保存信号帧 */
        sp -= sizeof(struct signal_frame);
        sp &= ~0xF;  /* 16 字节对齐 */
        
        /* 验证用户栈空间可写 */
        if (!access_ok((void *)sp, sizeof(struct signal_frame) + 16)) {
            println("[SIGNAL] Error: User stack not accessible");
            return 0;
        }
        
        /* 复制信号帧到用户栈 */
        struct signal_frame *user_frame = (struct signal_frame *)sp;
        
        /* 使用安全的用户空间写入 */
        if (copy_to_user(&user_frame->sig, &frame.sig, sizeof(frame.sig)) != 0 ||
            copy_to_user(&user_frame->info, &frame.info, sizeof(frame.info)) != 0 ||
            copy_to_user(&user_frame->action, &frame.action, sizeof(frame.action)) != 0 ||
            copy_to_user(&user_frame->context, &frame.context, sizeof(frame.context)) != 0 ||
            copy_to_user(&user_frame->onstack, &frame.onstack, sizeof(frame.onstack)) != 0) {
            println("[SIGNAL] Error: Failed to write signal frame to user stack");
            return 0;
        }
        
        /* 设置返回地址为 sigreturn 代码 */
        /* 在用户栈上放置 sigreturn 系统调用的代码 */
        sp -= 16;  /* 为 sigreturn 代码留出空间 */
        uint8_t *sigret_code = (uint8_t *)sp;
        
        /* x86-64 sigreturn 代码:
         * mov rax, 15       ; SYS_rt_sigreturn (Linux ABI)
         * syscall
         */
        uint8_t code[] = {0x48, 0xC7, 0xC0, 0x0F, 0x00, 0x00, 0x00, 0x0F, 0x05};
        if (copy_to_user(sigret_code, code, sizeof(code)) != 0) {
            println("[SIGNAL] Error: Failed to write sigreturn code to user stack");
            return 0;
        }
        
        /* 设置返回地址 */
        context->rsp = sp;
        context->rcx = (uint64_t)sigret_code;  /* syscall 使用 rcx 作为返回地址 */
        
        /* 保存信号帧指针供 sigreturn 使用 */
        context->r10 = (uint64_t)user_frame;
        
        return 1;  /* 已处理信号 */
    }
    
    return 0;  /* 没有待处理信号 */
}

/* 检查是否有待处理信号 */
int signal_has_pending(struct signal_state *state) {
    if (state == NULL) return 0;
    
    /* 检查是否有非阻塞的待处理信号 */
    for (int sig = 1; sig < NSIG; sig++) {
        if (sigismember(&state->pending, sig) && 
            !sigismember(&state->blocked, sig)) {
            return 1;
        }
    }
    
    return 0;
}

/* 设置信号动作 */
int signal_action(struct signal_state *state, int sig,
                   const struct sigaction *act, struct sigaction *oldact) {
    if (state == NULL || sig <= 0 || sig >= NSIG) return -1;
    
    /* SIGKILL 和 SIGSTOP 不能被捕获或忽略 */
    if (sig == SIGKILL || sig == SIGSTOP) {
        return -1;
    }
    
    spin_lock(&state->lock);
    
    if (oldact != NULL) {
        *oldact = state->actions[sig];
    }
    
    if (act != NULL) {
        state->actions[sig] = *act;
    }
    
    spin_unlock(&state->lock);
    
    return 0;
}

/* 信号掩码操作 */
int signal_procmask(struct signal_state *state, int how,
                     const sigset_t *set, sigset_t *oldset) {
    if (state == NULL) return -1;
    
    spin_lock(&state->lock);
    
    if (oldset != NULL) {
        *oldset = state->blocked;
    }
    
    if (set != NULL) {
        switch (how) {
            case SIG_BLOCK:
                state->blocked.sig[0] |= set->sig[0];
                state->blocked.sig[1] |= set->sig[1];
                break;
                
            case SIG_UNBLOCK:
                state->blocked.sig[0] &= ~set->sig[0];
                state->blocked.sig[1] &= ~set->sig[1];
                break;
                
            case SIG_SETMASK:
                state->blocked = *set;
                break;
                
            default:
                spin_unlock(&state->lock);
                return -1;
        }
        
        /* 不能阻塞 SIGKILL 和 SIGSTOP */
        sigdelset(&state->blocked, SIGKILL);
        sigdelset(&state->blocked, SIGSTOP);
    }
    
    spin_unlock(&state->lock);
    
    return 0;
}

/* 打印信号信息 */
void signal_print_info(siginfo_t *info) {
    if (info == NULL) {
        println("NULL siginfo");
        return;
    }
    
    print("Signal: ");
    print(signal_name(info->si_signo));
    print(" (");
    print_dec(info->si_signo);
    println(")");
    
    print("  Code: ");
    print_dec(info->si_code);
    println("");
    
    if (info->si_code == SI_USER) {
        print("  From PID: ");
        print_dec(info->_sifields._kill._pid);
        print(" UID: ");
        print_dec(info->_sifields._kill._uid);
        println("");
    }
    
    if (info->si_signo == SIGSEGV || info->si_signo == SIGBUS ||
        info->si_signo == SIGFPE || info->si_signo == SIGILL) {
        print("  Address: ");
        print_hex((uint64_t)info->_sifields._sigfault._addr);
        println("");
    }
}

/* 信号返回 */
int signal_return(struct signal_frame *frame) {
    if (frame == NULL) return -1;
    
    /* 恢复上下文 */
    /* 这通常由 sigreturn 系统调用执行 */
    
    /* 恢复信号掩码 */
    struct process *proc = process_get_current();
    if (proc) {
        struct sigaction *act = frame->action;
        
        /* 清除信号掩码 */
        sigdelset(&proc->signals->blocked, frame->sig);
        
        /* 如果设置了 SA_RESETHAND，重置为默认 */
        if (act->sa_flags & SA_RESETHAND) {
            act->sa_handler = SIG_DFL;
        }
        
        /* 清除备用栈标志 */
        if (frame->onstack) {
            proc->signals->altstack.ss_flags &= ~SS_ONSTACK;
        }
    }
    
    return 0;
}

/* ========== 信号安全增强功能 ========== */

/* 信号统计 */
struct signal_stats {
    uint64_t sent_count;
    uint64_t delivered_count;
    uint64_t dropped_count;
    uint64_t handler_calls;
    uint64_t handler_errors;
    uint64_t max_pending;
};

static struct signal_stats sig_stats;

/* 信号安全检查表 - 定义哪些信号可以发送给哪些进程 */
static const int signal_security_matrix[NSIG][4] = {
    /* [信号][特权级要求] */
    [SIGKILL] = { 1, 0, 0, 0 },  /* 需要 root 或同用户 */
    [SIGSTOP] = { 1, 0, 0, 0 },
    [SIGCONT] = { 0, 0, 0, 0 },  /* 任何人可以发送 */
    /* 默认其他信号需要同用户或 root */
};

/* 检查信号发送权限 */
int signal_check_permission(struct process *sender, struct process *target, int sig) {
    if (sender == NULL || target == NULL) return -1;
    if (sig <= 0 || sig >= NSIG) return -1;
    
    /* 发送给自己的信号总是允许 */
    if (sender->pid == target->pid) return 0;
    
    /* 同父进程的子进程间可以互相发送信号 */
    if (sender->parent_pid == target->parent_pid) return 0;
    
    /* 检查权限 (简化版: root 可以发送任何信号) */
    /* 完整实现需要检查 uid/gid */
    
    return 0;
}

/* 原子信号发送 */
int signal_send_atomic(struct process *proc, int sig, siginfo_t *info) {
    if (proc == NULL) return -1;
    if (sig <= 0 || sig >= NSIG) return -1;
    
    /* 获取信号锁 */
    if (proc->signals == NULL) return -1;
    
    spin_lock(&proc->signals->lock);
    
    /* 检查挂起信号数量限制 */
    if (proc->signals->has_pending >= 64) {
        sig_stats.dropped_count++;
        spin_unlock(&proc->signals->lock);
        return -1;  /* 信号队列已满 */
    }
    
    /* 添加到挂起队列 */
    sigaddset(&proc->signals->pending, sig);
    proc->signals->has_pending++;
    
    /* 更新统计 */
    if (proc->signals->has_pending > sig_stats.max_pending) {
        sig_stats.max_pending = proc->signals->has_pending;
    }
    
    sig_stats.sent_count++;
    
    spin_unlock(&proc->signals->lock);
    
    return 0;
}

/* 批量信号处理 */
int signal_process_batch(struct process *proc, int max_signals) {
    if (proc == NULL || proc->signals == NULL) return -1;
    
    int processed = 0;
    
    spin_lock(&proc->signals->lock);
    
    for (int sig = 1; sig < NSIG && processed < max_signals; sig++) {
        if (sigismember(&proc->signals->pending, sig)) {
            /* 从挂起队列移除 */
            sigdelset(&proc->signals->pending, sig);
            if (proc->signals->has_pending > 0) {
                proc->signals->has_pending--;
            }
            
            /* 处理信号 */
            int action = default_actions[sig];
            
            /* 检查是否被忽略 */
            struct sigaction *act = &proc->signals->actions[sig];
            if (act->sa_handler == SIG_IGN) {
                continue;  /* 忽略信号 */
            }
            
            /* 检查是否被阻塞 */
            if (sigismember(&proc->signals->blocked, sig)) {
                /* 重新添加到挂起队列 */
                sigaddset(&proc->signals->pending, sig);
                proc->signals->has_pending++;
                continue;
            }
            
            /* 执行默认动作或用户处理程序 */
            if (act->sa_handler == SIG_DFL) {
                /* 默认动作 */
                switch (action) {
                    case SIG_ACT_TERM:
                        proc->state = PROCESS_STATE_ZOMBIE;
                        proc->exit_code = 128 + sig;
                        break;
                    case SIG_ACT_CORE:
                        proc->state = PROCESS_STATE_ZOMBIE;
                        proc->exit_code = 128 + sig;
                        break;
                    case SIG_ACT_STOP:
                        /* 停止进程 */
                        break;
                    case SIG_ACT_CONT:
                        /* 继续进程 */
                        break;
                }
            } else {
                /* 用户处理程序 */
                sig_stats.handler_calls++;
            }
            
            sig_stats.delivered_count++;
            processed++;
        }
    }
    
    spin_unlock(&proc->signals->lock);
    
    return processed;
}

/* 信号优先级排序 */
static int signal_priority[NSIG] = {
    [SIGKILL] = 100,
    [SIGSTOP] = 99,
    [SIGSEGV] = 90,
    [SIGBUS]  = 89,
    [SIGILL]  = 88,
    [SIGFPE]  = 87,
    [SIGTRAP] = 86,
    [SIGABRT] = 85,
    [SIGTERM] = 50,
    [SIGINT]  = 49,
    [SIGHUP]  = 48,
    /* 其他信号默认优先级为 0 */
};

/* 获取最高优先级挂起信号 */
int signal_get_highest_priority(struct process *proc) {
    if (proc == NULL || proc->signals == NULL) return 0;
    
    int highest = 0;
    int highest_prio = -1;
    
    for (int sig = 1; sig < NSIG; sig++) {
        if (sigismember(&proc->signals->pending, sig) &&
            !sigismember(&proc->signals->blocked, sig)) {
            if (signal_priority[sig] > highest_prio) {
                highest_prio = signal_priority[sig];
                highest = sig;
            }
        }
    }
    
    return highest;
}

/* 信号重试机制 */
#define SIGNAL_MAX_RETRIES  3

struct signal_retry {
    int sig;
    siginfo_t info;
    int retry_count;
    uint64_t next_retry_time;
};

#define MAX_SIGNAL_RETRIES  16
static struct signal_retry retry_queue[MAX_SIGNAL_RETRIES];
static int retry_count = 0;

int signal_queue_retry(int sig, siginfo_t *info) {
    if (retry_count >= MAX_SIGNAL_RETRIES) return -1;
    
    struct signal_retry *r = &retry_queue[retry_count++];
    r->sig = sig;
    if (info != NULL) {
        r->info = *info;
    }
    r->retry_count = 0;
    r->next_retry_time = 0;  /* 立即重试 */
    
    return 0;
}

/* 处理重试队列 */
void signal_process_retries(void) {
    for (int i = 0; i < retry_count; i++) {
        struct signal_retry *r = &retry_queue[i];
        
        if (r->retry_count >= SIGNAL_MAX_RETRIES) {
            /* 超过最大重试次数，丢弃信号 */
            sig_stats.dropped_count++;
            
            /* 移除该条目 */
            for (int j = i; j < retry_count - 1; j++) {
                retry_queue[j] = retry_queue[j + 1];
            }
            retry_count--;
            i--;
            continue;
        }
        
        /* 尝试重新发送 */
        struct process *proc = process_get_current();
        if (proc != NULL && signal_send_atomic(proc, r->sig, &r->info) == 0) {
            /* 成功，移除该条目 */
            for (int j = i; j < retry_count - 1; j++) {
                retry_queue[j] = retry_queue[j + 1];
            }
            retry_count--;
            i--;
        } else {
            r->retry_count++;
            r->next_retry_time = 1000000;  /* 1ms 后重试 */
        }
    }
}

/* 获取信号统计 */
void signal_get_stats(struct signal_stats *stats) {
    if (stats != NULL) {
        *stats = sig_stats;
    }
}

/* 重置信号统计 */
void signal_reset_stats(void) {
    sig_stats.sent_count = 0;
    sig_stats.delivered_count = 0;
    sig_stats.dropped_count = 0;
    sig_stats.handler_calls = 0;
    sig_stats.handler_errors = 0;
    sig_stats.max_pending = 0;
}

/* ========== 内核安全增强功能 ========== */

/* === 安全审计系统 === */

/* 审计事件类型 */
enum audit_event_type {
    AUDIT_SYS_OPEN,
    AUDIT_SYS_READ,
    AUDIT_SYS_WRITE,
    AUDIT_SYS_EXEC,
    AUDIT_SIGNAL_SEND,
    AUDIT_MEMORY_ALLOC,
    AUDIT_MEMORY_FREE,
    AUDIT_PRIVILEGE_ESCALATE,
    AUDIT_CAPABILITY_USE,
    AUDIT_MODULE_LOAD,
    AUDIT_MODULE_UNLOAD,
};

/* 审计记录 */
struct audit_record {
    uint64_t timestamp;
    enum audit_event_type type;
    uint32_t pid;
    uint32_t uid;
    uint32_t gid;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    int result;
    char comm[16];
};

#define AUDIT_BUFFER_SIZE   1024
static struct audit_record audit_buffer[AUDIT_BUFFER_SIZE];
static uint32_t audit_buffer_head;
static uint32_t audit_buffer_tail;
static spinlock_t audit_lock = SPINLOCK_INIT;
static int audit_enabled;

/* 初始化审计系统 */
void security_audit_init(void) {
    audit_buffer_head = 0;
    audit_buffer_tail = 0;
    audit_enabled = 0;
}

/* 启用/禁用审计 */
void security_audit_enable(int enable) {
    audit_enabled = enable;
}

/* 记录审计事件 */
void security_audit_log(enum audit_event_type type, uint64_t arg1, 
                        uint64_t arg2, uint64_t arg3, int result) {
    if (!audit_enabled) return;
    
    spin_lock(&audit_lock);
    
    uint32_t idx = audit_buffer_head % AUDIT_BUFFER_SIZE;
    
    audit_buffer[idx].timestamp = get_tick_count();
    audit_buffer[idx].type = type;
    audit_buffer[idx].pid = 0;  /* 当前进程 PID */
    audit_buffer[idx].uid = 0;
    audit_buffer[idx].gid = 0;
    audit_buffer[idx].arg1 = arg1;
    audit_buffer[idx].arg2 = arg2;
    audit_buffer[idx].arg3 = arg3;
    audit_buffer[idx].result = result;
    
    audit_buffer_head++;
    
    if (audit_buffer_head - audit_buffer_tail > AUDIT_BUFFER_SIZE) {
        audit_buffer_tail++;
    }
    
    spin_unlock(&audit_lock);
}

/* 获取审计记录 */
int security_audit_get(struct audit_record *records, int max_count) {
    if (records == NULL) return 0;
    
    spin_lock(&audit_lock);
    
    int count = 0;
    uint32_t idx = audit_buffer_tail;
    
    while (idx < audit_buffer_head && count < max_count) {
        records[count] = audit_buffer[idx % AUDIT_BUFFER_SIZE];
        idx++;
        count++;
    }
    
    spin_unlock(&audit_lock);
    
    return count;
}

/* === 权限检查增强 === */

/* Capability 定义 */
#define CAP_CHOWN           0
#define CAP_DAC_OVERRIDE    1
#define CAP_DAC_READ_SEARCH 2
#define CAP_FOWNER          3
#define CAP_FSETID          4
#define CAP_KILL            5
#define CAP_SETGID          6
#define CAP_SETUID          7
#define CAP_SETPCAP         8
#define CAP_NET_BIND_SERVICE 9
#define CAP_NET_RAW         10
#define CAP_SYS_ADMIN       11
#define CAP_SYS_BOOT        12
#define CAP_SYS_CHROOT      13
#define CAP_SYS_MODULE      14
#define CAP_SYS_TIME        15

#define CAP_MAX             32

/* 检查 capability */
int security_check_capability(uint32_t cap) {
    if (cap >= CAP_MAX) return 0;
    
    /* 简化实现：内核线程拥有所有 capability */
    return 1;
}

/* 检查文件访问权限 */
int security_file_permission(const char *path, int mode) {
    if (path == NULL) return -1;
    
    /* 记录审计 */
    security_audit_log(AUDIT_SYS_OPEN, (uint64_t)path, mode, 0, 0);
    
    /* 简化实现：允许所有访问 */
    return 0;
}

/* 检查进程间信号发送权限 */
int security_signal_permission(uint32_t sender_pid, uint32_t target_pid, int sig) {
    /* 记录审计 */
    security_audit_log(AUDIT_SIGNAL_SEND, sender_pid, target_pid, sig, 0);
    
    /* 检查 capability */
    if (security_check_capability(CAP_KILL)) {
        return 0;  /* 有 CAP_KILL 可以发送任意信号 */
    }
    
    /* 检查是否是同一用户 */
    /* 简化实现：允许同一进程发送信号给自己 */
    if (sender_pid == target_pid) {
        return 0;
    }
    
    /* 其他情况需要进一步检查 */
    return 0;
}

/* === 安全策略管理 === */

/* 安全策略类型 */
enum policy_type {
    POLICY_ALLOW,
    POLICY_DENY,
    POLICY_AUDIT,
};

/* 安全策略规则 */
struct security_rule {
    char subject[32];       /* 主体 (进程/用户) */
    char object[64];        /* 客体 (文件/资源) */
    char operation[16];     /* 操作 */
    enum policy_type policy;
    int priority;
    int enabled;
};

#define MAX_SECURITY_RULES  64
static struct security_rule security_rules[MAX_SECURITY_RULES];
static int security_rule_count;

/* 初始化安全策略 */
void security_policy_init(void) {
    security_rule_count = 0;
    
    for (int i = 0; i < MAX_SECURITY_RULES; i++) {
        security_rules[i].enabled = 0;
    }
}

/* 添加安全规则 */
int security_add_rule(const char *subject, const char *object, 
                      const char *operation, enum policy_type policy, int priority) {
    if (security_rule_count >= MAX_SECURITY_RULES) return -1;
    
    struct security_rule *rule = &security_rules[security_rule_count];
    
    /* 复制字段 */
    int i;
    for (i = 0; i < 31 && subject[i]; i++) rule->subject[i] = subject[i];
    rule->subject[i] = '\0';
    
    for (i = 0; i < 63 && object[i]; i++) rule->object[i] = object[i];
    rule->object[i] = '\0';
    
    for (i = 0; i < 15 && operation[i]; i++) rule->operation[i] = operation[i];
    rule->operation[i] = '\0';
    
    rule->policy = policy;
    rule->priority = priority;
    rule->enabled = 1;
    
    security_rule_count++;
    
    return 0;
}

/* 检查安全策略 */
int security_check_policy(const char *subject, const char *object, 
                          const char *operation) {
    /* 遍历规则，按优先级排序检查 */
    for (int i = 0; i < security_rule_count; i++) {
        struct security_rule *rule = &security_rules[i];
        
        if (!rule->enabled) continue;
        
        /* 简化匹配 */
        int match = 1;
        
        /* 检查主体 (支持通配符 *) */
        if (rule->subject[0] != '*' && 
            strcmp(rule->subject, subject) != 0) {
            match = 0;
        }
        
        /* 检查客体 */
        if (rule->object[0] != '*' && 
            strcmp(rule->object, object) != 0) {
            match = 0;
        }
        
        /* 检查操作 */
        if (rule->operation[0] != '*' && 
            strcmp(rule->operation, operation) != 0) {
            match = 0;
        }
        
        if (match) {
            switch (rule->policy) {
                case POLICY_ALLOW:
                    return 0;
                case POLICY_DENY:
                    return -1;
                case POLICY_AUDIT:
                    security_audit_log(AUDIT_PRIVILEGE_ESCALATE, 
                                       (uint64_t)subject, (uint64_t)object, 
                                       (uint64_t)operation, 0);
                    return 0;
            }
        }
    }
    
    /* 默认允许 */
    return 0;
}

/* === 入侵检测 === */

/* 异常行为类型 */
enum anomaly_type {
    ANOMALY_BRUTE_FORCE,        /* 暴力破解 */
    ANOMALY_PRIVILEGE_ESCALATE, /* 权限提升尝试 */
    ANOMALY_RESOURCE_EXHAUST,   /* 资源耗尽 */
    ANOMALY_UNUSUAL_ACCESS,     /* 异常访问模式 */
    ANOMALY_SUSPICIOUS_SYSCALL, /* 可疑系统调用 */
};

/* 异常记录 */
struct anomaly_record {
    uint64_t timestamp;
    enum anomaly_type type;
    uint32_t pid;
    uint32_t severity;      /* 1-10 */
    char description[64];
    int count;
};

#define MAX_ANOMALIES       32
static struct anomaly_record anomalies[MAX_ANOMALIES];
static int anomaly_count;

/* 检测入侵行为 */
int security_detect_intrusion(void) {
    int detected = 0;
    
    /* 检查信号发送频率 */
    if (sig_stats.sent_count > 1000) {
        /* 可能是暴力破解 */
        if (anomaly_count < MAX_ANOMALIES) {
            anomalies[anomaly_count].timestamp = get_tick_count();
            anomalies[anomaly_count].type = ANOMALY_BRUTE_FORCE;
            anomalies[anomaly_count].pid = 0;
            anomalies[anomaly_count].severity = 5;
            anomalies[anomaly_count].count = sig_stats.sent_count;
            anomaly_count++;
            detected = 1;
        }
    }
    
    /* 检查内存分配频率 */
    /* TODO: 添加更多检测 */
    
    return detected;
}

/* 获取异常记录 */
int security_get_anomalies(struct anomaly_record *records, int max_count) {
    if (records == NULL) return 0;
    
    int count = anomaly_count < max_count ? anomaly_count : max_count;
    
    for (int i = 0; i < count; i++) {
        records[i] = anomalies[i];
    }
    
    return count;
}

/* === 安全沙箱 === */

/* 沙箱配置 */
struct sandbox_config {
    uint32_t allowed_syscalls[8];   /* 允许的系统调用位图 */
    char allowed_paths[16][64];     /* 允许访问的路径 */
    char denied_paths[16][64];      /* 禁止访问的路径 */
    uint64_t memory_limit;          /* 内存限制 */
    uint64_t cpu_limit;             /* CPU 时间限制 */
    int network_isolated;           /* 网络隔离 */
    int enabled;
};

#define MAX_SANDBOXES   8
static struct sandbox_config sandboxes[MAX_SANDBOXES];

/* 初始化沙箱 */
void sandbox_init(void) {
    for (int i = 0; i < MAX_SANDBOXES; i++) {
        sandboxes[i].enabled = 0;
        sandboxes[i].memory_limit = 0;
        sandboxes[i].cpu_limit = 0;
        sandboxes[i].network_isolated = 0;
    }
}

/* 创建沙箱 */
int sandbox_create(uint64_t memory_limit, uint64_t cpu_limit, int network_isolated) {
    for (int i = 0; i < MAX_SANDBOXES; i++) {
        if (!sandboxes[i].enabled) {
            sandboxes[i].memory_limit = memory_limit;
            sandboxes[i].cpu_limit = cpu_limit;
            sandboxes[i].network_isolated = network_isolated;
            sandboxes[i].enabled = 1;
            return i;
        }
    }
    return -1;
}

/* 检查沙箱限制 */
int sandbox_check_syscall(int sandbox_id, int syscall_num) {
    if (sandbox_id < 0 || sandbox_id >= MAX_SANDBOXES) return 0;
    if (!sandboxes[sandbox_id].enabled) return 0;
    
    /* 检查系统调用是否允许 */
    int word = syscall_num / 32;
    int bit = syscall_num % 32;
    
    if (word < 8 && (sandboxes[sandbox_id].allowed_syscalls[word] & (1 << bit))) {
        return 0;  /* 允许 */
    }
    
    /* 默认禁止 */
    return -1;
}

/* === 访问控制列表 (ACL) === */

/* ACL 条目 */
struct acl_entry {
    uint32_t subject_id;    /* 主体 ID (用户/组) */
    uint32_t permissions;   /* 权限位 */
    int is_group;           /* 是否是组 */
};

#define MAX_ACL_ENTRIES    16

/* ACL 结构 */
struct acl {
    struct acl_entry entries[MAX_ACL_ENTRIES];
    int count;
    uint32_t default_mask;
};

/* 初始化 ACL */
void acl_init(struct acl *acl) {
    if (acl == NULL) return;
    
    acl->count = 0;
    acl->default_mask = 0;  /* 默认无权限 */
}

/* 添加 ACL 条目 */
int acl_add_entry(struct acl *acl, uint32_t subject_id, uint32_t permissions, int is_group) {
    if (acl == NULL || acl->count >= MAX_ACL_ENTRIES) return -1;
    
    struct acl_entry *entry = &acl->entries[acl->count];
    entry->subject_id = subject_id;
    entry->permissions = permissions;
    entry->is_group = is_group;
    
    acl->count++;
    
    return 0;
}

/* 检查 ACL 权限 */
int acl_check_permission(struct acl *acl, uint32_t subject_id, uint32_t required_perms, int is_group) {
    if (acl == NULL) return -1;
    
    /* 遍历 ACL 条目 */
    for (int i = 0; i < acl->count; i++) {
        struct acl_entry *entry = &acl->entries[i];
        
        if (entry->subject_id == subject_id && entry->is_group == is_group) {
            if ((entry->permissions & required_perms) == required_perms) {
                return 0;  /* 权限匹配 */
            }
        }
    }
    
    /* 检查默认权限 */
    if ((acl->default_mask & required_perms) == required_perms) {
        return 0;
    }
    
    return -1;  /* 权限不足 */
}

/* === 安全统计 === */

struct security_stats {
    uint64_t audit_events;
    uint64_t policy_checks;
    uint64_t policy_denials;
    uint64_t anomaly_detections;
    uint64_t sandbox_violations;
};

static struct security_stats sec_stats;

/* 获取安全统计 */
void security_get_stats(struct security_stats *stats) {
    if (stats != NULL) {
        *stats = sec_stats;
    }
}

/* 打印安全报告 */
void security_print_report(void) {
    println("");
    println("╔════════════════════════════════════════════════════╗");
    println("║           Zratro OS Security Report                ║");
    println("╚════════════════════════════════════════════════════╝");
    
    println("");
    println("--- Audit Statistics ---");
    print("Audit events logged: ");
    print_dec(sec_stats.audit_events);
    println("");
    
    println("");
    println("--- Policy Enforcement ---");
    print("Policy checks: ");
    print_dec(sec_stats.policy_checks);
    println("");
    
    print("Policy denials: ");
    print_dec(sec_stats.policy_denials);
    println("");
    
    println("");
    println("--- Intrusion Detection ---");
    print("Anomalies detected: ");
    print_dec(sec_stats.anomaly_detections);
    println("");
    
    println("");
    println("--- Sandbox Status ---");
    int active_sandboxes = 0;
    for (int i = 0; i < MAX_SANDBOXES; i++) {
        if (sandboxes[i].enabled) active_sandboxes++;
    }
    print("Active sandboxes: ");
    print_dec(active_sandboxes);
    println("");
    
    print("Sandbox violations: ");
    print_dec(sec_stats.sandbox_violations);
    println("");
    
    println("");
    println("════════════════════════════════════════════════════");
}

/* === 安全模块初始化 === */

void security_init(void) {
    println("[SEC] Initializing security subsystem...");
    
    security_audit_init();
    security_policy_init();
    sandbox_init();
    
    /* 添加默认安全规则 */
    security_add_rule("*", "*", "*", POLICY_ALLOW, 0);
    
    println("[SEC] Security subsystem initialized");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：高级信号处理增强
 * 
 * 特性：
 * - 实时信号支持增强
 * - 信号处理性能分析
 * - 信号延迟统计
 * - 信号风暴检测
 * - 高级诊断报告
 * ═══════════════════════════════════════════════════════════════════════════ */

/* === 实时信号增强 === */

#define SIGRT_FIRST    SIGRTMIN
#define SIGRT_LAST     SIGRTMAX
#define SIGRT_COUNT    (SIGRT_LAST - SIGRT_FIRST + 1)

/* 实时信号队列增强 */
struct rt_signal_queue {
    struct sigqueue *head;
    struct sigqueue *tail;
    uint32_t count;
    uint32_t limit;
    uint64_t total_queued;
    uint64_t total_delivered;
};

static struct rt_signal_queue rt_queues[SIGRT_COUNT];

/* 初始化实时信号队列 */
void rt_signal_init(void) {
    for (int i = 0; i < SIGRT_COUNT; i++) {
        rt_queues[i].head = NULL;
        rt_queues[i].tail = NULL;
        rt_queues[i].count = 0;
        rt_queues[i].limit = 64;  /* 每个实时信号最多排队 64 个 */
        rt_queues[i].total_queued = 0;
        rt_queues[i].total_delivered = 0;
    }
}

/* 发送实时信号 */
int rt_signal_send(pid_t pid, int sig, const union sigval value, int flags) {
    if (sig < SIGRT_FIRST || sig > SIGRT_LAST) return -1;
    
    struct process *proc = process_get_by_pid(pid);
    if (proc == NULL) return -1;
    
    int idx = sig - SIGRT_FIRST;
    
    /* 检查队列限制 */
    if (flags & SIG_QUEUE_LIMIT && rt_queues[idx].count >= rt_queues[idx].limit) {
        return -1;  /* 队列已满 */
    }
    
    /* 创建信号信息 */
    siginfo_t info = {0};
    info.si_signo = sig;
    info.si_code = SI_QUEUE;
    info._sifields._rt._pid = 0;  /* 发送者 PID */
    info._sifields._rt._uid = 0;  /* 发送者 UID */
    info._sifields._rt._sigval = value;
    
    /* 排队 */
    int ret = signal_send_info(pid, sig, &info);
    if (ret == 0) {
        rt_queues[idx].count++;
        rt_queues[idx].total_queued++;
    }
    
    return ret;
}

/* 接收实时信号 */
int rt_signal_receive(pid_t pid, int sig, siginfo_t *info, int flags) {
    if (info == NULL) return -1;
    
    struct process *proc = process_get_by_pid(pid);
    if (proc == NULL) return -1;
    
    /* 检查是否有排队的实时信号 */
    int idx = sig - SIGRT_FIRST;
    if (idx >= 0 && idx < SIGRT_COUNT) {
        if (rt_queues[idx].count > 0) {
            rt_queues[idx].count--;
            rt_queues[idx].total_delivered++;
        }
    }
    
    return 0;
}

/* 获取实时信号统计 */
void rt_signal_get_stats(int sig, uint32_t *queued, uint32_t *delivered, uint32_t *pending) {
    if (sig >= SIGRT_FIRST && sig <= SIGRT_LAST) {
        int idx = sig - SIGRT_FIRST;
        if (queued) *queued = rt_queues[idx].total_queued;
        if (delivered) *delivered = rt_queues[idx].total_delivered;
        if (pending) *pending = rt_queues[idx].count;
    }
}

/* === 信号处理性能分析 === */

/* 信号处理时间统计 */
struct signal_perf {
    uint64_t handler_time_ns[NSIG];     /* 每个信号的处理时间 */
    uint64_t handler_calls[NSIG];       /* 每个信号的调用次数 */
    uint64_t avg_time_ns[NSIG];         /* 平均处理时间 */
    uint64_t max_time_ns[NSIG];         /* 最大处理时间 */
    uint64_t min_time_ns[NSIG];         /* 最小处理时间 */
    uint64_t total_time_ns;             /* 总处理时间 */
};

static struct signal_perf sig_perf;

/* 记录信号处理时间 */
void signal_record_handler_time(int sig, uint64_t time_ns) {
    if (sig <= 0 || sig >= NSIG) return;
    
    sig_perf.handler_time_ns[sig] += time_ns;
    sig_perf.handler_calls[sig]++;
    sig_perf.total_time_ns += time_ns;
    
    /* 更新平均值 */
    sig_perf.avg_time_ns[sig] = sig_perf.handler_time_ns[sig] / sig_perf.handler_calls[sig];
    
    /* 更新最大/最小值 */
    if (time_ns > sig_perf.max_time_ns[sig] || sig_perf.max_time_ns[sig] == 0) {
        sig_perf.max_time_ns[sig] = time_ns;
    }
    if (time_ns < sig_perf.min_time_ns[sig] || sig_perf.min_time_ns[sig] == 0) {
        sig_perf.min_time_ns[sig] = time_ns;
    }
}

/* 获取信号性能统计 */
void signal_get_perf_stats(int sig, uint64_t *calls, uint64_t *avg_time, 
                           uint64_t *max_time, uint64_t *min_time) {
    if (sig > 0 && sig < NSIG) {
        if (calls) *calls = sig_perf.handler_calls[sig];
        if (avg_time) *avg_time = sig_perf.avg_time_ns[sig];
        if (max_time) *max_time = sig_perf.max_time_ns[sig];
        if (min_time) *min_time = sig_perf.min_time_ns[sig];
    }
}

/* === 信号延迟统计 === */

/* 信号延迟记录 */
struct signal_latency {
    uint64_t send_time;         /* 发送时间 */
    uint64_t deliver_time;      /* 交付时间 */
    uint64_t latency_ns;        /* 延迟(纳秒) */
    int sig;
    pid_t pid;
};

#define MAX_LATENCY_RECORDS    256

static struct signal_latency latency_records[MAX_LATENCY_RECORDS];
static uint32_t latency_record_idx;
static spinlock_t latency_lock = SPINLOCK_INIT;

/* 记录信号延迟 */
void signal_record_latency(int sig, pid_t pid, uint64_t send_time) {
    uint64_t deliver_time = get_tick_count();
    
    spin_lock(&latency_lock);
    
    uint32_t idx = latency_record_idx % MAX_LATENCY_RECORDS;
    latency_records[idx].send_time = send_time;
    latency_records[idx].deliver_time = deliver_time;
    latency_records[idx].latency_ns = deliver_time - send_time;
    latency_records[idx].sig = sig;
    latency_records[idx].pid = pid;
    latency_record_idx++;
    
    spin_unlock(&latency_lock);
}

/* 计算延迟统计 */
void signal_get_latency_stats(uint64_t *avg_latency, uint64_t *max_latency, 
                              uint64_t *min_latency, uint64_t *count) {
    uint64_t total = 0;
    uint64_t max_l = 0;
    uint64_t min_l = (uint64_t)-1;
    uint64_t c = latency_record_idx < MAX_LATENCY_RECORDS ? latency_record_idx : MAX_LATENCY_RECORDS;
    
    for (uint32_t i = 0; i < c; i++) {
        uint64_t l = latency_records[i].latency_ns;
        total += l;
        if (l > max_l) max_l = l;
        if (l < min_l) min_l = l;
    }
    
    if (avg_latency) *avg_latency = c > 0 ? total / c : 0;
    if (max_latency) *max_latency = max_l;
    if (min_latency) *min_latency = c > 0 ? min_l : 0;
    if (count) *count = c;
}

/* === 信号风暴检测 === */

/* 信号风暴阈值 */
#define STORM_THRESHOLD_SHORT   100     /* 短时间内信号数 */
#define STORM_THRESHOLD_LONG    1000    /* 长时间内信号数 */
#define STORM_WINDOW_SHORT      1000000  /* 1ms (纳秒) */
#define STORM_WINDOW_LONG       1000000000ULL  /* 1s (纳秒) */

/* 风暴检测状态 */
struct storm_detector {
    uint64_t short_window_count;
    uint64_t short_window_start;
    uint64_t long_window_count;
    uint64_t long_window_start;
    int storm_detected;
    int storm_level;    /* 0-3, 越高越严重 */
};

static struct storm_detector storm_det;

/* 检测信号风暴 */
int signal_detect_storm(int sig) {
    uint64_t now = get_tick_count();
    
    /* 更新短窗口计数 */
    if (now - storm_det.short_window_start > STORM_WINDOW_SHORT) {
        storm_det.short_window_count = 0;
        storm_det.short_window_start = now;
    }
    storm_det.short_window_count++;
    
    /* 更新长窗口计数 */
    if (now - storm_det.long_window_start > STORM_WINDOW_LONG) {
        storm_det.long_window_count = 0;
        storm_det.long_window_start = now;
    }
    storm_det.long_window_count++;
    
    /* 检测风暴 */
    int storm = 0;
    if (storm_det.short_window_count > STORM_THRESHOLD_SHORT) {
        storm = 1;
        storm_det.storm_level = 3;  /* 严重风暴 */
    } else if (storm_det.long_window_count > STORM_THRESHOLD_LONG) {
        storm = 1;
        storm_det.storm_level = 2;  /* 中等风暴 */
    } else if (storm_det.short_window_count > STORM_THRESHOLD_SHORT / 2) {
        storm = 1;
        storm_det.storm_level = 1;  /* 轻微风暴 */
    } else {
        storm_det.storm_level = 0;
    }
    
    storm_det.storm_detected = storm;
    
    return storm;
}

/* 获取风暴状态 */
int signal_get_storm_level(void) {
    return storm_det.storm_level;
}

/* === 信号处理优化 === */

/* 信号批处理 */
struct signal_batch {
    int signals[16];
    siginfo_t infos[16];
    int count;
};

/* 添加信号到批处理 */
int signal_batch_add(struct signal_batch *batch, int sig, siginfo_t *info) {
    if (batch == NULL || batch->count >= 16) return -1;
    
    batch->signals[batch->count] = sig;
    if (info != NULL) {
        batch->infos[batch->count] = *info;
    }
    batch->count++;
    
    return 0;
}

/* 执行信号批处理 */
int signal_batch_process(struct signal_batch *batch, struct signal_state *state) {
    if (batch == NULL || state == NULL) return -1;
    
    int processed = 0;
    
    for (int i = 0; i < batch->count; i++) {
        int sig = batch->signals[i];
        
        /* 按优先级排序处理 */
        if (!sigismember(&state->blocked, sig)) {
            /* 处理信号 */
            processed++;
        }
    }
    
    batch->count = 0;
    return processed;
}

/* === 信号高级诊断 === */

/* 信号诊断信息 */
struct signal_diagnostic {
    struct signal_stats stats;
    struct signal_perf perf;
    uint64_t avg_latency;
    uint64_t max_latency;
    int storm_level;
    int active_handlers;
    int pending_count;
};

/* 获取信号诊断信息 */
void signal_get_diagnostic(struct signal_diagnostic *diag) {
    if (diag == NULL) return;
    
    /* 复制统计 */
    diag->stats = sig_stats;
    diag->perf = sig_perf;
    
    /* 获取延迟统计 */
    signal_get_latency_stats(&diag->avg_latency, &diag->max_latency, NULL, NULL);
    
    /* 获取风暴状态 */
    diag->storm_level = storm_det.storm_level;
    
    /* 计算活动处理程序数量 */
    diag->active_handlers = 0;
    for (int i = 1; i < NSIG; i++) {
        if (sig_perf.handler_calls[i] > 0) {
            diag->active_handlers++;
        }
    }
    
    /* 计算待处理数量 */
    diag->pending_count = 0;
    struct process *proc = process_get_current();
    if (proc != NULL && proc->signals != NULL) {
        for (int i = 1; i < NSIG; i++) {
            if (sigismember(&proc->signals->pending, i)) {
                diag->pending_count++;
            }
        }
    }
}

/* 打印信号诊断报告 */
void signal_print_diagnostic_report(void) {
    println("");
    println("╔════════════════════════════════════════════════════════════╗");
    println("║           Zratro OS Signal Diagnostic Report               ║");
    println("╚════════════════════════════════════════════════════════════╝");
    
    /* 基本统计 */
    println("");
    println("=== 基本统计 ===");
    
    print("发送次数: ");
    print_dec(sig_stats.sent_count);
    println("");
    
    print("交付次数: ");
    print_dec(sig_stats.delivered_count);
    println("");
    
    print("丢弃次数: ");
    print_dec(sig_stats.dropped_count);
    println("");
    
    print("处理程序调用: ");
    print_dec(sig_stats.handler_calls);
    println("");
    
    /* 延迟统计 */
    println("");
    println("=== 延迟统计 ===");
    
    uint64_t avg_latency, max_latency;
    signal_get_latency_stats(&avg_latency, &max_latency, NULL, NULL);
    
    print("平均延迟: ");
    print_dec(avg_latency / 1000);
    println(" us");
    
    print("最大延迟: ");
    print_dec(max_latency / 1000);
    println(" us");
    
    /* 风暴状态 */
    println("");
    println("=== 风暴检测 ===");
    
    const char *storm_levels[] = {
        "无", "轻微", "中等", "严重"
    };
    print("风暴等级: ");
    println(storm_levels[storm_det.storm_level]);
    
    /* 热门信号 */
    println("");
    println("=== 热门信号 ===");
    
    int top_sigs[3] = {0};
    uint64_t top_counts[3] = {0};
    
    for (int i = 1; i < NSIG; i++) {
        uint64_t c = sig_perf.handler_calls[i];
        for (int j = 0; j < 3; j++) {
            if (c > top_counts[j]) {
                /* 移动后面的 */
                for (int k = 2; k > j; k--) {
                    top_counts[k] = top_counts[k - 1];
                    top_sigs[k] = top_sigs[k - 1];
                }
                top_counts[j] = c;
                top_sigs[j] = i;
                break;
            }
        }
    }
    
    for (int i = 0; i < 3; i++) {
        if (top_counts[i] > 0) {
            print("  ");
            print(signal_name(top_sigs[i]));
            print(": ");
            print_dec(top_counts[i]);
            print(" 次, 平均 ");
            print_dec(sig_perf.avg_time_ns[top_sigs[i]] / 1000);
            println(" us");
        }
    }
    
    /* 实时信号状态 */
    println("");
    println("=== 实时信号 ===");
    
    for (int i = 0; i < 3 && i < SIGRT_COUNT; i++) {
        uint32_t queued, delivered, pending;
        rt_signal_get_stats(SIGRT_FIRST + i, &queued, &delivered, &pending);
        
        if (queued > 0) {
            print("SIGRT");
            print_dec(i);
            print(": queued=");
            print_dec(queued);
            print(", pending=");
            print_dec(pending);
            println("");
        }
    }
    
    /* 优化建议 */
    println("");
    println("=== 优化建议 ===");
    
    if (storm_det.storm_level >= 2) {
        println("- 检测到信号风暴，建议优化信号处理逻辑");
    }
    
    if (sig_stats.dropped_count > 0) {
        println("- 存在信号丢弃，建议增加信号队列容量");
    }
    
    if (avg_latency > 1000000) {
        println("- 信号延迟较高，建议检查处理程序性能");
    }
    
    println("");
    println("════════════════════════════════════════════════════════════");
}

/* === 信号高级功能 === */

/* 信号定时发送 */
struct timed_signal {
    uint64_t trigger_time;
    int sig;
    pid_t pid;
    siginfo_t info;
    int active;
};

#define MAX_TIMED_SIGNALS   16
static struct timed_signal timed_signals[MAX_TIMED_SIGNALS];

/* 安排定时信号 */
int signal_schedule(uint64_t delay_ns, int sig, pid_t pid, siginfo_t *info) {
    for (int i = 0; i < MAX_TIMED_SIGNALS; i++) {
        if (!timed_signals[i].active) {
            timed_signals[i].trigger_time = get_tick_count() + delay_ns;
            timed_signals[i].sig = sig;
            timed_signals[i].pid = pid;
            if (info != NULL) {
                timed_signals[i].info = *info;
            }
            timed_signals[i].active = 1;
            return i;
        }
    }
    return -1;
}

/* 取消定时信号 */
int signal_cancel(int timer_id) {
    if (timer_id >= 0 && timer_id < MAX_TIMED_SIGNALS) {
        timed_signals[timer_id].active = 0;
        return 0;
    }
    return -1;
}

/* 处理定时信号 */
void signal_process_timed(void) {
    uint64_t now = get_tick_count();
    
    for (int i = 0; i < MAX_TIMED_SIGNALS; i++) {
        if (timed_signals[i].active && now >= timed_signals[i].trigger_time) {
            /* 发送信号 */
            signal_send_info(timed_signals[i].pid, timed_signals[i].sig, 
                           &timed_signals[i].info);
            timed_signals[i].active = 0;
        }
    }
}

/* 信号合并 (合并相同信号) */
int signal_merge_pending(struct signal_state *state) {
    if (state == NULL) return -1;
    
    int merged = 0;
    
    /* 对于标准信号，多个相同信号只保留一个 */
    for (int sig = 1; sig < SIGRTMIN; sig++) {
        int count = 0;
        struct sigqueue *q = state->queue_head;
        struct sigqueue *prev = NULL;
        
        while (q != NULL) {
            if (q->info.si_signo == sig) {
                count++;
                if (count > 1) {
                    /* 移除重复信号 */
                    if (prev != NULL) {
                        prev->next = q->next;
                    } else {
                        state->queue_head = q->next;
                    }
                    
                    if (q == state->queue_tail) {
                        state->queue_tail = prev;
                    }
                    
                    state->queue_count--;
                    struct sigqueue *next = q->next;
                    kfree(q);
                    q = next;
                    merged++;
                    continue;
                }
            }
            prev = q;
            q = q->next;
        }
    }
    
    return merged;
}

/* 信号优先级调整 */
int signal_set_priority(int sig, int priority) {
    if (sig <= 0 || sig >= NSIG) return -1;
    if (priority < 0 || priority > 100) return -1;
    
    signal_priority[sig] = priority;
    return 0;
}

/* 信号掩码继承 */
int signal_inherit_mask(struct process *parent, struct process *child) {
    if (parent == NULL || child == NULL) return -1;
    if (parent->signals == NULL || child->signals == NULL) return -1;
    
    /* 复制信号掩码 */
    child->signals->blocked = parent->signals->blocked;
    
    /* 复制信号动作 (可选) */
    for (int i = 1; i < NSIG; i++) {
        /* SIGCHLD 默认忽略 */
        if (i == SIGCHLD) continue;
        child->signals->actions[i] = parent->signals->actions[i];
    }
    
    return 0;
}
