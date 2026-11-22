/*
 * TCP BBR v2 Congestion Control - ZTS Module
 *
 * Adapted from Linux kernel net/ipv4/tcp_bbr.c
 * Google's BBR (Bottleneck Bandwidth and RTT) congestion control.
 *
 * Unlike loss-based algorithms (Reno/CUBIC), BBR models the network
 * path and paces packets at the estimated bottleneck bandwidth,
 * achieving high throughput with low latency.
 *
 * State machine:
 *   STARTUP -> DRAIN -> PROBE_BW <-> PROBE_RTT
 *
 * Priority: 0x00000002
 * Category: NETWORK
 */

#include "zts_adapt.h"

/* ============================================================
 * BBR Constants
 * ============================================================ */

#define BBR_SCALE 8
#define BBR_UNIT (1 << BBR_SCALE)

#define BW_SCALE 24
#define BW_UNIT (1 << BW_SCALE)

#define BBR_MAX_BW_FILTER_WIN 10    /* windowed max bw over 10 round trips */
#define BBR_MIN_RTT_FILTER_WIN 1000 /* min rtt window in ms */

/* BBR gains (fixed-point, scaled by BBR_UNIT) */
#define BBR_DEFAULT_GAIN_BW  BBR_UNIT  /* 1.0 */
#define BBR_DEFAULT_GAIN_CWN BBR_UNIT  /* 1.0 */
#define BBR_STARTUP_GAIN_BW  (2885)    /* 2.888 = ln(2)/0.24 */
#define BBR_STARTUP_GAIN_CWN (2885)
#define BBR_DRAIN_GAIN       (790)     /* 0.79 = 1/1.25 */
#define BBR_PROBE_RTT_GAIN   (395)     /* 0.5 = half bw */

/* BBR pacing rate */
#define BBR_MIN_PACING_RATE  65535
#define BBR_MAX_PACING_RATE  0xFFFFFFFF

/* BBR modes */
enum bbr_mode {
    BBR_STARTUP = 0,
    BBR_DRAIN,
    BBR_PROBE_BW,
    BBR_PROBE_RTT,
};

/* PROBE_BW cycle phases */
enum bbr_phase {
    BBR_PHASE_DOWN = 0,
    BBR_PHASE_CRUISE,
    BBR_PHASE_REFILL,
    BBR_PHASE_UP,
};

/* ============================================================
 * BBR Control Block
 * ============================================================ */

struct bbr_state {
    /* Bandwidth estimation */
    u64 bottleneck_bw;          /* bytes/sec */
    u32 bw_filter[BBR_MAX_BW_FILTER_WIN];
    u32 bw_index;

    /* RTT estimation */
    u32 min_rtt_us;             /* microseconds */
    u64 min_rtt_stamp;          /* jiffies */

    /* State machine */
    u32 mode;
    u32 phase;
    u32 cycle_idx;

    /* Window */
    u32 cwnd;
    u32 target_cwnd;
    u32 pacing_rate;

    /* Delivery */
    u32 delivered;
    u32 delivered_at;
    u64 first_tx_mstamp;

    /* Counters */
    u32 round_start;
    u32 rtt_cnt;
    u32 probe_rtt_rounds;

    /* Startup */
    u32 full_bw_cnt;
    u32 full_bw;
    u32 full_bw_now;

    /* Stats */
    u32 total_acks;
    u32 total_losses;
};

/* ============================================================
 * Module Global
 * ============================================================ */

static struct bbr_state *bbr_global = NULL;
static u32 bbr_init_count = 0;
static u32 bbr_exec_count = 0;

/* ============================================================
 * BBR Helper Functions
 * ============================================================ */

/* Fixed-point multiply: (a * b) >> BBR_SCALE */
static u32 bbr_mul(u32 a, u32 b) {
    u64 tmp = (u64)a * b;
    return (u32)(tmp >> BBR_SCALE);
}

/* Update windowed max bandwidth */
static void bbr_update_bw(struct bbr_state *ca, u32 sample_bw) {
    ca->bw_filter[ca->bw_index % BBR_MAX_BW_FILTER_WIN] = sample_bw;

    /* Find max */
    u32 max_bw = 0;
    for (int i = 0; i < BBR_MAX_BW_FILTER_WIN; i++) {
        if (ca->bw_filter[i] > max_bw)
            max_bw = ca->bw_filter[i];
    }
    ca->bottleneck_bw = max_bw;
    ca->bw_index++;
}

/* Update min RTT */
static void bbr_update_min_rtt(struct bbr_state *ca, u32 rtt_us) {
    u64 now = get_jiffies_64();

    if (ca->min_rtt_us == 0 || rtt_us < ca->min_rtt_us) {
        ca->min_rtt_us = rtt_us;
        ca->min_rtt_stamp = now;
    }
}

/* Calculate pacing rate */
static u32 bbr_calc_pacing_rate(struct bbr_state *ca, u32 bw, u32 gain) {
    u64 rate = (u64)bw * gain;
    rate = rate / BBR_UNIT;
    if (rate == 0) rate = BBR_MIN_PACING_RATE;
    return (u32)rate;
}

/* Calculate target cwnd: BDP * cwnd_gain */
static u32 bbr_calc_target_cwnd(struct bbr_state *ca, u32 bw, u32 gain) {
    /* BDP = bandwidth * min_rtt */
    u64 bdp = ((u64)bw * ca->min_rtt_us) / 1000000;
    u64 target = (bdp * gain) / BBR_UNIT;
    if (target < 4) target = 4;  /* minimum 4 packets */
    return (u32)target;
}

/* ============================================================
 * BBR State Machine
 * ============================================================ */

/* STARTUP: ramp up quickly to fill pipe */
static void bbr_startup_step(struct bbr_state *ca, u32 sample_bw, u32 rtt_us) {
    u32 gain_bw = BBR_STARTUP_GAIN_BW;
    u32 gain_cwn = BBR_STARTUP_GAIN_CWN;

    ca->pacing_rate = bbr_calc_pacing_rate(ca, sample_bw, gain_bw);
    ca->target_cwnd = bbr_calc_target_cwnd(ca, sample_bw, gain_cwn);

    /* Check if pipe is full: bw increase < 25% over 3 rounds */
    if (sample_bw > ca->full_bw) {
        ca->full_bw = sample_bw;
        ca->full_bw_cnt = 0;
    } else {
        ca->full_bw_cnt++;
        if (ca->full_bw_cnt >= 3) {
            ca->mode = BBR_DRAIN;
            printk("[ZTS:BBR] STARTUP -> DRAIN (bw=%lu)", (unsigned long)ca->bottleneck_bw);
        }
    }
}

/* DRAIN: drain queue built during startup */
static void bbr_drain_step(struct bbr_state *ca, u32 sample_bw) {
    u32 gain = BBR_DRAIN_GAIN;

    ca->pacing_rate = bbr_calc_pacing_rate(ca, sample_bw, gain);
    ca->target_cwnd = bbr_calc_target_cwnd(ca, sample_bw, BBR_UNIT);

    /* Exit drain when inflight drops */
    if (ca->delivered >= ca->target_cwnd) {
        ca->mode = BBR_PROBE_BW;
        ca->phase = BBR_PHASE_DOWN;
        ca->cycle_idx = 0;
        printk("[ZTS:BBR] DRAIN -> PROBE_BW");
    }
}

/* PROBE_BW: cycle through gain values to probe bandwidth */
static const u32 bbr_pacing_gain[] = {
    1250,  /* UP: probe up (1.25) */
    900,   /* DOWN: back off (0.9) */
    1000,  /* CRUISE (1.0) */
    1000,
    1000,
    1000,
    1000,
};

static void bbr_probe_bw_step(struct bbr_state *ca, u32 sample_bw, u32 rtt_us) {
    u32 gain = bbr_pacing_gain[ca->cycle_idx % 7];

    ca->pacing_rate = bbr_calc_pacing_rate(ca, sample_bw, gain);
    ca->target_cwnd = bbr_calc_target_cwnd(ca, sample_bw, BBR_UNIT);

    /* Check for PROBE_RTT condition: min_rtt not refreshed for 10s */
    u64 now = get_jiffies_64();
    if (now - ca->min_rtt_stamp > 10000) {
        ca->mode = BBR_PROBE_RTT;
        ca->probe_rtt_rounds = 0;
        printk("[ZTS:BBR] PROBE_BW -> PROBE_RTT");
        return;
    }

    /* Advance cycle */
    if (ca->round_start) {
        ca->cycle_idx++;
        if (ca->cycle_idx >= 7) ca->cycle_idx = 0;
    }
}

/* PROBE_RTT: cut inflight to probe min_rtt */
static void bbr_probe_rtt_step(struct bbr_state *ca) {
    ca->pacing_rate = bbr_calc_pacing_rate(ca, ca->bottleneck_bw, BBR_PROBE_RTT_GAIN);
    ca->target_cwnd = 4;  /* minimum cwnd */

    if (ca->round_start) {
        ca->probe_rtt_rounds++;
        if (ca->probe_rtt_rounds >= 2) {
            /* Check if we should restart or go to PROBE_BW */
            if (ca->full_bw > 0) {
                ca->mode = BBR_PROBE_BW;
            } else {
                ca->mode = BBR_STARTUP;
            }
            printk("[ZTS:BBR] PROBE_RTT -> %s",
                   ca->mode == BBR_PROBE_BW ? "PROBE_BW" : "STARTUP");
        }
    }
}

/* ============================================================
 * BBR Main Logic
 * ============================================================ */

/* Input from TCP layer */
struct bbr_input {
    u32 event;              /* 0=ACK, 1=LOSS, 2=TIMEOUT */
    u32 delivered;          /* segments delivered since last ACK */
    u32 rtt_us;             /* RTT sample in microseconds */
    u32 current_cwnd;       /* current congestion window */
    u32 inflight;           /* bytes in flight */
};

/* Output to TCP layer */
struct bbr_output {
    u32 new_cwnd;           /* new congestion window */
    u32 pacing_rate;        /* suggested pacing rate */
    u32 target_cwnd;        /* target cwnd */
    u32 mode;               /* current BBR mode */
};

static void bbr_main(struct bbr_state *ca,
                     struct bbr_input *inp,
                     struct bbr_output *out) {
    u32 sample_bw = 0;

    /* Calculate delivery rate */
    if (inp->delivered > 0 && inp->rtt_us > 0) {
        sample_bw = (inp->delivered * 1000000) / inp->rtt_us;
    }

    /* Update estimations */
    if (sample_bw > 0)
        bbr_update_bw(ca, sample_bw);
    if (inp->rtt_us > 0)
        bbr_update_min_rtt(ca, inp->rtt_us);

    ca->delivered += inp->delivered;
    ca->round_start = (inp->event == 0) ? 1 : 0;

    /* State machine */
    switch (ca->mode) {
    case BBR_STARTUP:
        bbr_startup_step(ca, sample_bw, inp->rtt_us);
        break;
    case BBR_DRAIN:
        bbr_drain_step(ca, sample_bw);
        break;
    case BBR_PROBE_BW:
        bbr_probe_bw_step(ca, sample_bw, inp->rtt_us);
        break;
    case BBR_PROBE_RTT:
        bbr_probe_rtt_step(ca);
        break;
    }

    /* Handle loss */
    if (inp->event == 1) {
        ca->total_losses++;
        ca->cwnd = (ca->cwnd * 7) / 10;  /* reduce by 30% */
        if (ca->cwnd < 4) ca->cwnd = 4;
    }

    /* Output results */
    out->new_cwnd = ca->target_cwnd;
    out->pacing_rate = ca->pacing_rate;
    out->target_cwnd = ca->target_cwnd;
    out->mode = ca->mode;
}

/* ============================================================
 * ZTS Module Interface
 * ============================================================ */

static int bbr_init(void) {
    bbr_global = (struct bbr_state *)kzalloc(sizeof(struct bbr_state));
    if (!bbr_global) {
        printk("[ZTS:BBR] Failed to allocate memory");
        return -ENOMEM;
    }

    /* Initialize state */
    memset(bbr_global, 0, sizeof(*bbr_global));
    bbr_global->mode = BBR_STARTUP;
    bbr_global->min_rtt_us = 0;
    bbr_global->cwnd = 10;

    bbr_init_count = 0;
    bbr_exec_count = 0;

    printk("[ZTS:TCP_BBR] BBR v2 congestion control loaded");
    printk("[ZTS:TCP_BBR] Mode: STARTUP, Min RTT: probing...");

    return 0;
}

static int bbr_exec(void *input, void *output, void *ctx) {
    if (!input || !output || !bbr_global) {
        printk("[ZTS:BBR] Invalid pointers");
        return -EINVAL;
    }

    struct bbr_input *inp = (struct bbr_input *)input;
    struct bbr_output *out = (struct bbr_output *)output;

    bbr_exec_count++;
    bbr_global->total_acks++;

    bbr_main(bbr_global, inp, out);

    return 0;
}

static void bbr_cleanup(void) {
    if (bbr_global) {
        printk("[ZTS:TCP_BBR] Stats: acks=%d, losses=%d",
               bbr_global->total_acks, bbr_global->total_losses);
        kfree(bbr_global);
        bbr_global = NULL;
    }
    printk("[ZTS:TCP_BBR] BBR unloaded");
}

ZTS_MODULE_INIT(bbr_init)
ZTS_MODULE_EXEC(bbr_exec)
ZTS_MODULE_CLEANUP(bbr_cleanup)
