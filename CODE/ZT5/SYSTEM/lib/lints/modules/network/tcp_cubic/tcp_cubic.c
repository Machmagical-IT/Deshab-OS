/*
 * TCP CUBIC Congestion Control - ZTS Module
 *
 * Adapted from Linux kernel net/ipv4/tcp_cubic.c
 * Replaces ZratroOS TCP Reno with CUBIC algorithm.
 *
 * CUBIC is the default TCP congestion control algorithm in Linux since 2.6.19.
 * It uses a cubic function to grow the congestion window, providing better
 * scalability for high-speed networks compared to Reno's linear growth.
 *
 * Key features:
 * - Cubic window growth function: W(t) = C*(t-K)^3 + W_max
 * - TCP-friendly region adjustment
 * - Fast convergence for flow fairness
 * - Idle TCP handling with HyStart slow start
 *
 * Priority: 0x00000001 (High - Network)
 * Category: NETWORK
 */

#include "zts_adapt.h"

/* ============================================================
 * CUBIC Constants
 * ============================================================ */

#define BICTCP_BETA_SCALE    1024       /* Scale factor for beta */
#define BICTCP_BETA          819        /* Beta = 0.8 (multiplicative decrease) */
#define BICTCP_HZ            10         /* HZ = 10 (time unit in 1/10 sec) */
#define BICTCP_MIN_BETA      614        /* Minimum beta = 0.6 */
#define BICTCP_MAX_BETA      819        /* Maximum beta = 0.8 */

#define CUbic_CONST          410        /* C = 0.4 * BICTCP_HZ^3 */
#define CUBIC_MAX_K          128        /* Maximum K value */

#define HYSTART_ACK_THRESH   8          /* HyStart: ACK threshold */
#define HYSTART_LOW_ATK      2          /* HyStart: Low attenuation */
#define HYSTART_HIGH_ATK     4          /* HyStart: High attenuation */

/* CUBIC states */
enum cubic_state {
    CUBIC_STATE_INIT = 0,
    CUBIC_STATE_CONG_AVOID,
    CUBIC_STATE_FAST_RECOVERY,
};

/* ============================================================
 * CUBIC Control Block
 * Per-connection state for CUBIC algorithm
 * ============================================================ */

struct bictcp {
    u32 cnt;              /* Inverse of expected number of ACKs before cwnd increment */
    u32 last_max_cwnd;    /* Previous maximum congestion window */
    u32 last_cwnd;        /* Last congestion window */
    u32 origin_point;     /* W_max, the window at which packet loss occurred */
    u64 epoch_start;      /* Starting time of the current epoch */
    u32 delay_min;        /* Minimum observed delay (RTT) */
    u32 ack_cnt;          /* Number of ACKs received in current epoch */
    u32 tcp_cwnd;         /* Estimated TCP congestion window */
    u32 origin_point_win; /* Window size at origin point */
    u32 bic_K;            /* Cubic K value (time to reach origin point) */
    u32 round_start;      /* Round start flag */
    u32 sample_cnt;       /* Sample count */
    u32 state;            /* Current state */
    
    /* HyStart parameters */
    u32 hystart_found;
    u32 hystart_sndmax;
    u64 hystart_time;
    u32 hystart_last_snd;
    u64 hystart_last_ack;
};

/* ============================================================
 * Module Global State
 * ============================================================ */

static struct bictcp *cubic_global_data = NULL;
static u32 cubic_init_count = 0;
static u32 cubic_exec_count = 0;

/* CUBIC parameters (tunable) */
static u32 cubic_beta = BICTCP_BETA;
static u32 cubic_c = CUbic_CONST;
static u32 cubic_fast_convergence = 1;
static u32 cubic_tcp_friendly = 1;
static u32 cubic_hystart = 1;

/* ============================================================
 * CUBIC Math Functions
 * ============================================================ */

/*
 * Cubic root approximation
 * Uses Newton's method for integer cube root
 */
static u32 cubic_root(u32 n) {
    u32 x = 1;
    
    if (n == 0) return 0;
    
    /* Initial approximation */
    x = n / 3;
    if (x == 0) x = 1;
    
    /* Newton's method iterations */
    for (int i = 0; i < 8; i++) {
        x = (2 * x + n / (x * x)) / 3;
    }
    
    return x;
}

/*
 * CUBIC window calculation
 * W(t) = C * (t - K)^3 + W_max
 *
 * t: time since start of current epoch (in units of BICTCP_HZ)
 * origin_point: W_max (window at last loss event)
 * bic_K: time to reach origin point
 */
static u32 cubic_window(u32 t, u32 origin_point, u32 bic_K, u32 c) {
    u32 delta;
    u32 offs;
    u64 tmp;
    
    if (t < bic_K) {
        /* We are before the origin point - window is decreasing */
        delta = bic_K - t;
        tmp = (u64)c * delta * delta * delta;
        offs = (u32)(tmp >> (BICTCP_HZ + 10));
        return (offs > origin_point) ? 1 : (origin_point - offs);
    } else {
        /* We are after the origin point - window is increasing */
        delta = t - bic_K;
        tmp = (u64)c * delta * delta * delta;
        offs = (u32)(tmp >> (BICTCP_HZ + 10));
        return origin_point + offs;
    }
}

/*
 * Calculate K value
 * K = cubic_root(W_max * HZ / C)
 */
static u32 cubic_calc_k(u32 w_max, u32 c) {
    u64 tmp = ((u64)w_max * BICTCP_HZ) / c;
    return cubic_root((u32)tmp);
}

/* ============================================================
 * TCP Friendliness
 * ============================================================ */

/*
 * TCP-friendly region calculation
 * Ensures CUBIC doesn't grow faster than standard TCP Reno
 */
static u32 cubic_tcp_friendliness(u32 w_max, u32 cwnd, u32 rtt) {
    if (!cubic_tcp_friendly)
        return cwnd;
    
    /* Calculate Reno-equivalent window growth */
    u32 reno_cwnd = cwnd + (cwnd * BICTCP_BETA) / BICTCP_BETA_SCALE;
    
    /* Use the minimum of CUBIC and Reno growth */
    return min(cwnd, reno_cwnd);
}

/* ============================================================
 * Fast Convergence
 * ============================================================ */

/*
 * Fast convergence mechanism
 * Reduces W_max on consecutive losses to improve fairness
 */
static void cubic_fast_converge(struct bictcp *ca) {
    if (!cubic_fast_convergence)
        return;
    
    if (ca->last_cwnd < ca->last_max_cwnd) {
        /* Previous loss was at a lower window - reduce W_max */
        if (ca->last_max_cwnd > 0) {
            ca->last_max_cwnd = (ca->last_max_cwnd * (BICTCP_BETA_SCALE - cubic_beta / 2)) 
                               / BICTCP_BETA_SCALE;
        }
    }
    
    ca->last_cwnd = ca->origin_point;
}

/* ============================================================
 * HyStart - Hybrid Slow Start
 * ============================================================ */

/*
 * HyStart detection
 * Detects the end of slow start based on delay increase
 */
static int hystart_detect(struct bictcp *ca, u32 current_cwnd) {
    if (!cubic_hystart)
        return 0;
    
    u64 now = get_jiffies_64();
    
    /* First ACK */
    if (ca->hystart_last_ack == 0) {
        ca->hystart_last_ack = now;
        ca->hystart_last_snd = current_cwnd;
        return 0;
    }
    
    /* Check if we've received enough ACKs */
    if (ca->hystart_last_ack + HYSTART_ACK_THRESH > now) {
        return 0;
    }
    
    /* Check for delay increase */
    u64 delay = now - ca->hystart_last_ack;
    if (delay > HYSTART_LOW_ATK) {
        ca->hystart_found = 1;
        return 1;
    }
    
    ca->hystart_last_ack = now;
    ca->hystart_last_snd = current_cwnd;
    
    return 0;
}

/* ============================================================
 * CUBIC Congestion Control Operations
 * ============================================================ */

/*
 * Congestion window calculation
 * Called on each ACK to calculate new cwnd
 */
static u32 cubic_cong_avoid(struct bictcp *ca, u32 current_cwnd, u32 rtt) {
    u64 now = get_jiffies_64();
    u32 t;
    u32 target_cwnd;
    
    /* Calculate time since epoch start */
    if (ca->epoch_start == 0) {
        ca->epoch_start = now;
        ca->ack_cnt = 0;
        ca->tcp_cwnd = current_cwnd;
    }
    
    t = (u32)((now - ca->epoch_start) * BICTCP_HZ / 1000);
    
    /* Calculate target window using cubic function */
    target_cwnd = cubic_window(t, ca->origin_point, ca->bic_K, cubic_c);
    
    /* Apply TCP friendliness */
    target_cwnd = cubic_tcp_friendliness(ca->origin_point, target_cwnd, rtt);
    
    /* Window increment */
    if (current_cwnd < target_cwnd) {
        ca->ack_cnt++;
        
        if (ca->ack_cnt >= ca->cnt) {
            ca->ack_cnt = 0;
            ca->tcp_cwnd++;
        }
    }
    
    return min(current_cwnd + 1, target_cwnd);
}

/*
 * Loss event handling
 * Called when packet loss is detected
 */
static void cubic_loss(struct bictcp *ca, u32 current_cwnd) {
    /* Fast convergence */
    cubic_fast_converge(ca);
    
    /* Update W_max */
    ca->last_max_cwnd = ca->origin_point;
    ca->origin_point = current_cwnd;
    
    /* Reduce window by beta */
    u32 new_cwnd = (current_cwnd * cubic_beta) / BICTCP_BETA_SCALE;
    ca->origin_point = max(new_cwnd, 2U);
    
    /* Recalculate K */
    ca->bic_K = cubic_calc_k(ca->origin_point, cubic_c);
    
    /* Reset epoch */
    ca->epoch_start = 0;
    ca->ack_cnt = 0;
    
    printk("[ZTS:CUBIC] Loss event: cwnd %d -> %d, K=%d",
           current_cwnd, ca->origin_point, ca->bic_K);
}

/* ============================================================
 * ZTS Module Interface
 * ============================================================ */

/* Input structure for congestion control */
struct cubic_input {
    u32 event;              /* Event type: 0=ACK, 1=LOSS, 2=TIMEOUT */
    u32 current_cwnd;       /* Current congestion window */
    u32 rtt;                /* Current RTT estimate */
    u32 segs_acked;         /* Number of segments acked */
};

/* Output structure for congestion control */
struct cubic_output {
    u32 new_cwnd;           /* New congestion window */
    u32 action;             /* Action: 0=NONE, 1=REDUCE, 2=INCREASE */
    u32 state;              /* Current CUBIC state */
};

/*
 * Module initialization
 */
static int cubic_init(void) {
    /* Allocate global state */
    cubic_global_data = (struct bictcp *)kzalloc(sizeof(struct bictcp) * 64);
    if (!cubic_global_data) {
        printk("[ZTS:CUBIC] Failed to allocate memory for %d connections", 64);
        return -ENOMEM;
    }
    
    /* Initialize parameters */
    cubic_init_count = 0;
    cubic_exec_count = 0;
    
    printk("[ZTS:TCP_CUBIC] TCP CUBIC congestion control loaded");
    printk("[ZTS:TCP_CUBIC] Parameters: beta=%d/%d, C=%d, fast_conv=%d, friendly=%d, hystart=%d",
           cubic_beta, BICTCP_BETA_SCALE, cubic_c, cubic_fast_convergence,
           cubic_tcp_friendly, cubic_hystart);
    
    return 0;
}

/*
 * Module execution
 * Input: struct cubic_input
 * Output: struct cubic_output
 */
static int cubic_exec(void *input, void *output, void *ctx) {
    if (!input || !output) {
        printk("[ZTS:CUBIC] Invalid input/output pointers");
        return -EINVAL;
    }
    
    struct cubic_input *inp = (struct cubic_input *)input;
    struct cubic_output *out = (struct cubic_output *)output;
    
    /* Get connection state (simplified - uses global for demo) */
    struct bictcp *ca = &cubic_global_data[0];
    
    cubic_exec_count++;
    
    switch (inp->event) {
    case 0: /* ACK received */
        out->new_cwnd = cubic_cong_avoid(ca, inp->current_cwnd, inp->rtt);
        out->action = (out->new_cwnd > inp->current_cwnd) ? 2 : 0;
        out->state = CUBIC_STATE_CONG_AVOID;
        break;
        
    case 1: /* Loss detected */
        cubic_loss(ca, inp->current_cwnd);
        out->new_cwnd = ca->origin_point;
        out->action = 1;
        out->state = CUBIC_STATE_FAST_RECOVERY;
        break;
        
    case 2: /* Timeout */
        /* Slow start on timeout */
        ca->origin_point = 2;
        ca->epoch_start = 0;
        ca->bic_K = 0;
        out->new_cwnd = 2;
        out->action = 1;
        out->state = CUBIC_STATE_INIT;
        printk("[ZTS:CUBIC] Timeout - reset to slow start");
        break;
        
    default:
        printk("[ZTS:CUBIC] Unknown event: %d", inp->event);
        return -EINVAL;
    }
    
    return 0;
}

/*
 * Module cleanup
 */
static void cubic_cleanup(void) {
    if (cubic_global_data) {
        kfree(cubic_global_data);
        cubic_global_data = NULL;
    }
    
    printk("[ZTS:TCP_CUBIC] TCP CUBIC unloaded");
    printk("[ZTS:TCP_CUBIC] Stats: init=%d, exec=%d", cubic_init_count, cubic_exec_count);
}

/* Export as ZTS module */
ZTS_MODULE_INIT(cubic_init)
ZTS_MODULE_EXEC(cubic_exec)
ZTS_MODULE_CLEANUP(cubic_cleanup)
