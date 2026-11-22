/*
 * Zratro OS - Performance Counters Implementation
 * 
 * 统一的性能监控实现
 */

#include "perf.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "task.h"
#include "smp.h"

/* 全局性能状态 */
struct perf_state perf_state;

/* 计数器名称 */
static const char *counter_names[PERF_COUNTER_COUNT] = {
    [PERF_SCHED_CONTEXT_SWITCHES] = "context_switches",
    [PERF_SCHED_TASK_CREATES] = "task_creates",
    [PERF_SCHED_TASK_DELETES] = "task_deletes",
    [PERF_SCHED_PREEMPTIONS] = "preemptions",
    [PERF_SCHED_LOAD_BALANCE] = "load_balance",
    [PERF_MEM_PAGE_FAULTS] = "page_faults",
    [PERF_MEM_PAGE_ALLOC] = "page_alloc",
    [PERF_MEM_PAGE_FREE] = "page_free",
    [PERF_MEM_CACHE_HITS] = "mem_cache_hits",
    [PERF_MEM_CACHE_MISSES] = "mem_cache_misses",
    [PERF_MEM_SLAB_ALLOC] = "slab_alloc",
    [PERF_MEM_SLAB_FREE] = "slab_free",
    [PERF_FS_READS] = "fs_reads",
    [PERF_FS_WRITES] = "fs_writes",
    [PERF_FS_CACHE_HITS] = "fs_cache_hits",
    [PERF_FS_CACHE_MISSES] = "fs_cache_misses",
    [PERF_NET_PACKETS_IN] = "net_packets_in",
    [PERF_NET_PACKETS_OUT] = "net_packets_out",
    [PERF_NET_BYTES_IN] = "net_bytes_in",
    [PERF_NET_BYTES_OUT] = "net_bytes_out",
    [PERF_NET_TCP_CONNECTIONS] = "tcp_connections",
    [PERF_NET_TCP_RETRANSMITS] = "tcp_retransmits",
    [PERF_IPC_MESSAGES] = "ipc_messages",
    [PERF_IPC_SIGNALS] = "ipc_signals",
    [PERF_RCU_GP_COUNT] = "rcu_gp_count",
    [PERF_RCU_CALLBACKS] = "rcu_callbacks",
    [PERF_IO_REQUESTS] = "io_requests",
    [PERF_IO_BYTES] = "io_bytes"
};

/* ========== 初始化 ========== */

int perf_init(void) {
    println("[PERF] Initializing performance monitoring...");
    
    /* 初始化计数器 */
    for (int i = 0; i < PERF_COUNTER_COUNT; i++) {
        perf_state.counters[i].value = 0;
        perf_state.counters[i].last_value = 0;
        perf_state.counters[i].rate_per_sec = 0;
        perf_state.counters[i].flags = 0;
        
        for (int j = 0; j < 31 && counter_names[i][j]; j++) {
            perf_state.counters[i].name[j] = counter_names[i][j];
        }
        perf_state.counters[i].name[31] = '\0';
        
        perf_state.samples[i].count = 0;
        perf_state.samples[i].total = 0;
        perf_state.samples[i].min = 0xFFFFFFFFFFFFFFFFULL;
        perf_state.samples[i].max = 0;
        perf_state.samples[i].last = 0;
    }
    
    /* 分配事件缓冲 */
    perf_state.event_capacity = 4096;
    perf_state.event_buffer = kmalloc(sizeof(struct perf_event) * perf_state.event_capacity);
    if (perf_state.event_buffer == NULL) {
        perf_state.event_capacity = 0;
    }
    perf_state.event_count = 0;
    perf_state.event_head = 0;
    perf_state.event_tail = 0;
    
    /* 初始化调试设置 */
    perf_state.debug_level = DEBUG_LEVEL_WARN;
    perf_state.debug_flags = DEBUG_FLAG_ENABLED | DEBUG_FLAG_STATS;
    perf_state.debug_filter[0] = '\0';
    
    /* 记录启动时间 */
    perf_state.start_time = get_tick_count();
    perf_state.last_update = perf_state.start_time;
    
    perf_state.lock = SPINLOCK_INIT;
    
    println("[PERF] Performance monitoring initialized");
    return 0;
}

void perf_shutdown(void) {
    if (perf_state.event_buffer != NULL) {
        kfree(perf_state.event_buffer);
        perf_state.event_buffer = NULL;
    }
    
    perf_state.event_capacity = 0;
    perf_state.event_count = 0;
}

/* ========== 计数器操作 ========== */

void perf_inc(enum perf_counter_type type) {
    if (type >= PERF_COUNTER_COUNT) return;
    
    __sync_fetch_and_add(&perf_state.counters[type].value, 1);
}

void perf_inc_by(enum perf_counter_type type, uint64_t value) {
    if (type >= PERF_COUNTER_COUNT) return;
    
    __sync_fetch_and_add(&perf_state.counters[type].value, value);
}

uint64_t perf_read(enum perf_counter_type type) {
    if (type >= PERF_COUNTER_COUNT) return 0;
    
    return perf_state.counters[type].value;
}

void perf_reset(enum perf_counter_type type) {
    if (type >= PERF_COUNTER_COUNT) return;
    
    spin_lock(&perf_state.lock);
    perf_state.counters[type].value = 0;
    perf_state.counters[type].last_value = 0;
    perf_state.counters[type].rate_per_sec = 0;
    spin_unlock(&perf_state.lock);
}

void perf_reset_all(void) {
    spin_lock(&perf_state.lock);
    
    for (int i = 0; i < PERF_COUNTER_COUNT; i++) {
        perf_state.counters[i].value = 0;
        perf_state.counters[i].last_value = 0;
        perf_state.counters[i].rate_per_sec = 0;
        
        perf_state.samples[i].count = 0;
        perf_state.samples[i].total = 0;
        perf_state.samples[i].min = 0xFFFFFFFFFFFFFFFFULL;
        perf_state.samples[i].max = 0;
    }
    
    perf_state.event_count = 0;
    perf_state.event_head = 0;
    perf_state.event_tail = 0;
    
    spin_unlock(&perf_state.lock);
}

void perf_update_rates(void) {
    uint64_t now = get_tick_count();
    uint64_t elapsed = now - perf_state.last_update;
    
    if (elapsed == 0) return;
    
    /* 计算每秒变化率 */
    for (int i = 0; i < PERF_COUNTER_COUNT; i++) {
        uint64_t delta = perf_state.counters[i].value - perf_state.counters[i].last_value;
        
        /* 转换为每秒 */
        uint64_t rate = (delta * 1000000000ULL) / elapsed;
        
        perf_state.counters[i].last_value = perf_state.counters[i].value;
        perf_state.counters[i].rate_per_sec = rate;
    }
    
    perf_state.last_update = now;
}

/* ========== 采样 ========== */

static uint64_t sample_start_times[PERF_COUNTER_COUNT];

void perf_sample_begin(enum perf_counter_type type) {
    if (type >= PERF_COUNTER_COUNT) return;
    
    sample_start_times[type] = get_tick_count();
}

uint64_t perf_sample_end(enum perf_counter_type type) {
    if (type >= PERF_COUNTER_COUNT) return 0;
    
    uint64_t elapsed = get_tick_count() - sample_start_times[type];
    perf_record_sample(type, elapsed);
    return elapsed;
}

void perf_record_sample(enum perf_counter_type type, uint64_t value) {
    if (type >= PERF_COUNTER_COUNT) return;
    
    spin_lock(&perf_state.lock);
    
    struct perf_sample *s = &perf_state.samples[type];
    s->count++;
    s->total += value;
    s->last = value;
    
    if (value < s->min) s->min = value;
    if (value > s->max) s->max = value;
    
    spin_unlock(&perf_state.lock);
}

void perf_get_sample_stats(enum perf_counter_type type, struct perf_sample *stats) {
    if (type >= PERF_COUNTER_COUNT || stats == NULL) return;
    
    spin_lock(&perf_state.lock);
    *stats = perf_state.samples[type];
    spin_unlock(&perf_state.lock);
}

/* ========== 事件记录 ========== */

void perf_event_record(enum perf_counter_type type, uint64_t value) {
    perf_event_record_ex(type, value, 0, 0);
}

void perf_event_record_ex(enum perf_counter_type type, uint64_t value,
                          uint64_t aux1, uint64_t aux2) {
    if (type >= PERF_COUNTER_COUNT) return;
    if (perf_state.event_buffer == NULL) return;
    
    spin_lock(&perf_state.lock);
    
    if (perf_state.event_count >= perf_state.event_capacity) {
        /* 缓冲已满，覆盖最旧事件 */
        perf_state.event_tail = (perf_state.event_tail + 1) % perf_state.event_capacity;
        perf_state.event_count--;
    }
    
    struct perf_event *event = &perf_state.event_buffer[perf_state.event_head];
    event->timestamp = get_tick_count();
    event->type = type;
    event->cpu = smp_processor_id();
    event->value = value;
    event->aux[0] = aux1;
    event->aux[1] = aux2;
    
    perf_state.event_head = (perf_state.event_head + 1) % perf_state.event_capacity;
    perf_state.event_count++;
    
    spin_unlock(&perf_state.lock);
}

int perf_event_get(struct perf_event *event) {
    if (event == NULL) return -1;
    if (perf_state.event_count == 0) return -1;
    
    spin_lock(&perf_state.lock);
    
    *event = perf_state.event_buffer[perf_state.event_tail];
    perf_state.event_tail = (perf_state.event_tail + 1) % perf_state.event_capacity;
    perf_state.event_count--;
    
    spin_unlock(&perf_state.lock);
    return 0;
}

void perf_event_clear(void) {
    spin_lock(&perf_state.lock);
    perf_state.event_count = 0;
    perf_state.event_head = 0;
    perf_state.event_tail = 0;
    spin_unlock(&perf_state.lock);
}

/* ========== 调试接口 ========== */

void debug_set_level(debug_level_t level) {
    if (level <= DEBUG_LEVEL_TRACE) {
        perf_state.debug_level = level;
    }
}

debug_level_t debug_get_level(void) {
    return perf_state.debug_level;
}

void debug_set_flags(uint32_t flags) {
    perf_state.debug_flags |= flags;
}

void debug_clear_flags(uint32_t flags) {
    perf_state.debug_flags &= ~flags;
}

void debug_set_filter(const char *filter) {
    if (filter == NULL) {
        perf_state.debug_filter[0] = '\0';
        return;
    }
    
    for (int i = 0; i < 63 && filter[i]; i++) {
        perf_state.debug_filter[i] = filter[i];
    }
    perf_state.debug_filter[63] = '\0';
}

void debug_print(debug_level_t level, const char *fmt, ...) {
    if (level > perf_state.debug_level) return;
    if (!(perf_state.debug_flags & DEBUG_FLAG_ENABLED)) return;
    
    /* 简化实现：直接打印 */
    /* 实际应支持格式化参数 */
    (void)fmt;
}

/* ========== 统计接口 ========== */

void perf_get_stats(struct perf_stats *stats) {
    if (stats == NULL) return;
    
    spin_lock(&perf_state.lock);
    
    /* CPU 统计 */
    stats->cpu_idle_time = 0;
    stats->cpu_busy_time = 0;
    stats->cpu_user_time = 0;
    stats->cpu_kernel_time = 0;
    stats->cpu_irq_time = 0;
    
    /* 内存统计 */
    stats->mem_total = 0;
    stats->mem_used = 0;
    stats->mem_cached = 0;
    stats->mem_free = 0;
    stats->mem_fragmentation = 0;
    
    /* 调度统计 */
    stats->sched_switches = perf_read(PERF_SCHED_CONTEXT_SWITCHES);
    stats->sched_latency_avg = 0;
    stats->sched_latency_max = 0;
    stats->runnable_tasks = 0;
    
    /* IO 统计 */
    stats->io_reads = perf_read(PERF_FS_READS);
    stats->io_writes = perf_read(PERF_FS_WRITES);
    stats->io_read_bytes = 0;
    stats->io_write_bytes = 0;
    stats->io_queue_depth = 0;
    
    /* 网络统计 */
    stats->net_rx_packets = perf_read(PERF_NET_PACKETS_IN);
    stats->net_tx_packets = perf_read(PERF_NET_PACKETS_OUT);
    stats->net_rx_bytes = perf_read(PERF_NET_BYTES_IN);
    stats->net_tx_bytes = perf_read(PERF_NET_BYTES_OUT);
    stats->net_errors = 0;
    
    spin_unlock(&perf_state.lock);
}

void perf_update_stats(void) {
    perf_update_rates();
}

void perf_print_report(void) {
    println("[PERF] Performance Report:");
    println("================================");
    
    /* 调度器统计 */
    println("Scheduler:");
    print("  Context switches: ");
    print_dec(perf_read(PERF_SCHED_CONTEXT_SWITCHES));
    println("");
    print("  Task creates: ");
    print_dec(perf_read(PERF_SCHED_TASK_CREATES));
    println("");
    print("  Task deletes: ");
    print_dec(perf_read(PERF_SCHED_TASK_DELETES));
    println("");
    print("  Preemptions: ");
    print_dec(perf_read(PERF_SCHED_PREEMPTIONS));
    println("");
    print("  Load balance: ");
    print_dec(perf_read(PERF_SCHED_LOAD_BALANCE));
    println("");
    
    /* 内存统计 */
    println("Memory:");
    print("  Page faults: ");
    print_dec(perf_read(PERF_MEM_PAGE_FAULTS));
    println("");
    print("  Page allocs: ");
    print_dec(perf_read(PERF_MEM_PAGE_ALLOC));
    println("");
    print("  Page frees: ");
    print_dec(perf_read(PERF_MEM_PAGE_FREE));
    println("");
    print("  Cache hits: ");
    print_dec(perf_read(PERF_MEM_CACHE_HITS));
    println("");
    print("  Cache misses: ");
    print_dec(perf_read(PERF_MEM_CACHE_MISSES));
    println("");
    
    /* 文件系统统计 */
    println("File System:");
    print("  Reads: ");
    print_dec(perf_read(PERF_FS_READS));
    println("");
    print("  Writes: ");
    print_dec(perf_read(PERF_FS_WRITES));
    println("");
    print("  Cache hits: ");
    print_dec(perf_read(PERF_FS_CACHE_HITS));
    println("");
    print("  Cache misses: ");
    print_dec(perf_read(PERF_FS_CACHE_MISSES));
    println("");
    
    /* 网络统计 */
    println("Network:");
    print("  Packets in: ");
    print_dec(perf_read(PERF_NET_PACKETS_IN));
    println("");
    print("  Packets out: ");
    print_dec(perf_read(PERF_NET_PACKETS_OUT));
    println("");
    print("  TCP connections: ");
    print_dec(perf_read(PERF_NET_TCP_CONNECTIONS));
    println("");
    
    println("================================");
}

void perf_print_counters(void) {
    println("[PERF] Counters:");
    
    for (int i = 0; i < PERF_COUNTER_COUNT; i++) {
        if (perf_state.counters[i].value > 0 ||
            perf_state.counters[i].rate_per_sec > 0) {
            print("  ");
            print(perf_state.counters[i].name);
            print(": ");
            print_dec(perf_state.counters[i].value);
            print(" (rate: ");
            print_dec(perf_state.counters[i].rate_per_sec);
            println("/s)");
        }
    }
}

/* ========== 剖析接口 ========== */

#define MAX_PROFILE_POINTS 64

struct profile_point {
    char name[32];
    uint64_t start_time;
    uint64_t total_time;
    uint64_t count;
};

static struct profile_point profile_points[MAX_PROFILE_POINTS];
static uint32_t profile_count = 0;
static spinlock_t profile_lock = SPINLOCK_INIT;

void perf_profile_start(const char *name) {
    spin_lock(&profile_lock);
    
    /* 查找或创建剖析点 */
    struct profile_point *pp = NULL;
    for (uint32_t i = 0; i < profile_count; i++) {
        int match = 1;
        for (int j = 0; j < 31 && (name[j] || profile_points[i].name[j]); j++) {
            if (name[j] != profile_points[i].name[j]) {
                match = 0;
                break;
            }
        }
        if (match) {
            pp = &profile_points[i];
            break;
        }
    }
    
    if (pp == NULL && profile_count < MAX_PROFILE_POINTS) {
        pp = &profile_points[profile_count++];
        for (int j = 0; j < 31 && name[j]; j++) {
            pp->name[j] = name[j];
        }
        pp->name[31] = '\0';
        pp->total_time = 0;
        pp->count = 0;
    }
    
    if (pp != NULL) {
        pp->start_time = get_tick_count();
    }
    
    spin_unlock(&profile_lock);
}

uint64_t perf_profile_end(const char *name) {
    uint64_t elapsed = 0;
    
    spin_lock(&profile_lock);
    
    for (uint32_t i = 0; i < profile_count; i++) {
        int match = 1;
        for (int j = 0; j < 31 && (name[j] || profile_points[i].name[j]); j++) {
            if (name[j] != profile_points[i].name[j]) {
                match = 0;
                break;
            }
        }
        if (match) {
            elapsed = get_tick_count() - profile_points[i].start_time;
            profile_points[i].total_time += elapsed;
            profile_points[i].count++;
            break;
        }
    }
    
    spin_unlock(&profile_lock);
    return elapsed;
}

/* ========== 追踪接口 ========== */

#define MAX_TRACE_POINTS 256

static struct trace_point trace_points[MAX_TRACE_POINTS];
static uint32_t trace_point_count = 0;
static spinlock_t trace_lock = SPINLOCK_INIT;

void trace_get_info(const char *name, struct trace_point *info) {
    if (name == NULL || info == NULL) return;
    
    spin_lock(&trace_lock);
    
    for (uint32_t i = 0; i < trace_point_count; i++) {
        int match = 1;
        for (int j = 0; j < 31 && (name[j] || trace_points[i].name[j]); j++) {
            if (name[j] != trace_points[i].name[j]) {
                match = 0;
                break;
            }
        }
        if (match) {
            *info = trace_points[i];
            break;
        }
    }
    
    spin_unlock(&trace_lock);
}

/* ========== 硬件性能计数器 ========== */

static uint64_t hw_perf_counters[HW_PERF_COUNT];
static int hw_perf_enabled[HW_PERF_COUNT];

int hw_perf_enable(enum hw_perf_type type) {
    if (type >= HW_PERF_COUNT) return -1;
    
    /* 使用 CPUID 检查是否支持 */
    /* 简化实现：直接启用 */
    hw_perf_enabled[type] = 1;
    hw_perf_counters[type] = 0;
    
    return 0;
}

void hw_perf_disable(enum hw_perf_type type) {
    if (type >= HW_PERF_COUNT) return;
    
    hw_perf_enabled[type] = 0;
}

uint64_t hw_perf_read(enum hw_perf_type type) {
    if (type >= HW_PERF_COUNT) return 0;
    
    /* 读取 MSR 或使用 RDPMC */
    /* 简化实现：返回软件计数器 */
    return hw_perf_counters[type];
}

void hw_perf_reset(enum hw_perf_type type) {
    if (type >= HW_PERF_COUNT) return;
    
    hw_perf_counters[type] = 0;
}

/* ========== 系统健康监控 ========== */

void health_get_metrics(struct health_metrics *metrics) {
    if (metrics == NULL) return;
    
    spin_lock(&perf_state.lock);
    
    uint64_t uptime = get_tick_count() - perf_state.start_time;
    metrics->uptime_seconds = uptime / 1000000000ULL;
    
    /* 简化实现 */
    metrics->cpu_load = 50;
    metrics->mem_usage = 40;
    metrics->io_wait = 10;
    metrics->swap_usage = 0;
    metrics->net_usage = 20;
    metrics->errors = 0;
    metrics->warnings = 0;
    
    spin_unlock(&perf_state.lock);
}

int health_check(void) {
    struct health_metrics metrics;
    health_get_metrics(&metrics);
    
    /* 检查各项指标 */
    if (metrics.cpu_load > 90) return -1;
    if (metrics.mem_usage > 90) return -1;
    if (metrics.errors > 100) return -1;
    
    return 0;  /* 健康 */
}

void health_print_report(void) {
    struct health_metrics metrics;
    health_get_metrics(&metrics);
    
    println("[HEALTH] System Health Report:");
    print("  CPU Load: ");
    print_dec(metrics.cpu_load);
    println("%");
    print("  Memory Usage: ");
    print_dec(metrics.mem_usage);
    println("%");
    print("  IO Wait: ");
    print_dec(metrics.io_wait);
    println("%");
    print("  Network Usage: ");
    print_dec(metrics.net_usage);
    println("%");
    print("  Uptime: ");
    print_dec(metrics.uptime_seconds);
    println(" seconds");
    
    if (health_check() == 0) {
        println("  Status: HEALTHY");
    } else {
        println("  Status: WARNING");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：实时性能分析 (Real-time Performance Analysis)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 性能分析配置 */
#define PERF_ANALYSIS_WINDOW    10000000ULL  /* 10ms 分析窗口 */
#define PERF_HOTSPOT_THRESHOLD  1000000ULL   /* 1ms 热点阈值 */

/* 热点记录 */
struct perf_hotspot {
    uint64_t address;           /* 代码地址 */
    uint64_t samples;           /* 采样次数 */
    uint64_t total_time;        /* 总时间 */
    char name[64];              /* 函数名 (如果有) */
};

#define MAX_HOTSPOTS    32

static struct perf_hotspot hotspots[MAX_HOTSPOTS];
static int hotspot_count = 0;

/* 记录热点 */
void perf_record_hotspot(uint64_t addr, uint64_t time) {
    /* 查找已存在的热点 */
    for (int i = 0; i < hotspot_count; i++) {
        if (hotspots[i].address == addr) {
            hotspots[i].samples++;
            hotspots[i].total_time += time;
            return;
        }
    }
    
    /* 添加新热点 */
    if (hotspot_count < MAX_HOTSPOTS) {
        hotspots[hotspot_count].address = addr;
        hotspots[hotspot_count].samples = 1;
        hotspots[hotspot_count].total_time = time;
        hotspot_count++;
    }
}

/* 获取热点列表 */
int perf_get_hotspots(struct perf_hotspot *list, int max_count) {
    if (list == NULL || max_count <= 0) return 0;
    
    int count = hotspot_count < max_count ? hotspot_count : max_count;
    for (int i = 0; i < count; i++) {
        list[i] = hotspots[i];
    }
    
    return count;
}

/* 清除热点 */
void perf_clear_hotspots(void) {
    hotspot_count = 0;
    memset(hotspots, 0, sizeof(hotspots));
}

/* 打印热点报告 */
void perf_print_hotspots(void) {
    println("=== Performance Hotspots ===");
    
    /* 按时间排序 */
    for (int i = 0; i < hotspot_count - 1; i++) {
        for (int j = i + 1; j < hotspot_count; j++) {
            if (hotspots[j].total_time > hotspots[i].total_time) {
                struct perf_hotspot temp = hotspots[i];
                hotspots[i] = hotspots[j];
                hotspots[j] = temp;
            }
        }
    }
    
    for (int i = 0; i < hotspot_count && i < 10; i++) {
        print("  ");
        print_dec(i + 1);
        print(": 0x");
        print_hex(hotspots[i].address);
        print(" - ");
        print_dec(hotspots[i].samples);
        print(" samples, ");
        print_dec(hotspots[i].total_time / 1000000);
        println(" ms total");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：性能追踪 (Performance Tracing)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 追踪事件类型 */
enum trace_event_type {
    TRACE_SCHED_SWITCH = 1,
    TRACE_SCHED_WAKEUP,
    TRACE_IRQ_ENTER,
    TRACE_IRQ_EXIT,
    TRACE_SYSCALL_ENTER,
    TRACE_SYSCALL_EXIT,
    TRACE_MEM_ALLOC,
    TRACE_MEM_FREE,
    TRACE_IO_START,
    TRACE_IO_END,
    TRACE_NET_SEND,
    TRACE_NET_RECV
};

/* 追踪记录 */
struct trace_record {
    uint64_t timestamp;
    uint32_t event_type;
    uint32_t cpu;
    uint64_t data[4];       /* 事件相关数据 */
};

#define TRACE_BUFFER_SIZE   8192

static struct trace_record trace_buffer[TRACE_BUFFER_SIZE];
static uint32_t trace_head = 0;
static uint32_t trace_tail = 0;
static int tracing_enabled = 0;

/* 启用追踪 */
void perf_trace_enable(int enable) {
    tracing_enabled = enable;
}

/* 记录追踪事件 */
void perf_trace_event(uint32_t type, uint64_t d0, uint64_t d1, 
                       uint64_t d2, uint64_t d3) {
    if (!tracing_enabled) return;
    
    while (__sync_lock_test_and_set(&trace_lock, 1)) {
        asm volatile("pause");
    }
    
    uint32_t next = (trace_head + 1) % TRACE_BUFFER_SIZE;
    if (next == trace_tail) {
        /* 缓冲区满，丢弃最旧的记录 */
        trace_tail = (trace_tail + 1) % TRACE_BUFFER_SIZE;
    }
    
    struct trace_record *rec = &trace_buffer[trace_head];
    rec->timestamp = get_tick_count();
    rec->event_type = type;
    rec->cpu = smp_processor_id();
    rec->data[0] = d0;
    rec->data[1] = d1;
    rec->data[2] = d2;
    rec->data[3] = d3;
    
    trace_head = next;
    __sync_lock_release(&trace_lock);
}

/* 读取追踪记录 */
int perf_trace_read(struct trace_record *records, int max_count) {
    if (records == NULL || max_count <= 0) return 0;
    
    while (__sync_lock_test_and_set(&trace_lock, 1)) {
        asm volatile("pause");
    }
    
    int count = 0;
    while (trace_tail != trace_head && count < max_count) {
        records[count++] = trace_buffer[trace_tail];
        trace_tail = (trace_tail + 1) % TRACE_BUFFER_SIZE;
    }
    
    __sync_lock_release(&trace_lock);
    return count;
}

/* 清除追踪缓冲 */
void perf_trace_clear(void) {
    while (__sync_lock_test_and_set(&trace_lock, 1)) {
        asm volatile("pause");
    }
    trace_head = 0;
    trace_tail = 0;
    __sync_lock_release(&trace_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：性能基准测试 (Performance Benchmarking)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 基准测试结果 */
struct benchmark_result {
    char name[64];
    uint64_t iterations;
    uint64_t total_time;
    uint64_t min_time;
    uint64_t max_time;
    uint64_t avg_time;
};

/* 运行基准测试 */
void perf_run_benchmark(const char *name, void (*func)(void *), 
                         void *arg, uint64_t iterations) {
    if (func == NULL || iterations == 0) return;
    
    struct benchmark_result result;
    strncpy(result.name, name, sizeof(result.name) - 1);
    result.name[sizeof(result.name) - 1] = '\0';
    result.iterations = iterations;
    result.total_time = 0;
    result.min_time = (uint64_t)-1;
    result.max_time = 0;
    
    for (uint64_t i = 0; i < iterations; i++) {
        uint64_t start = get_tick_count();
        func(arg);
        uint64_t end = get_tick_count();
        
        uint64_t elapsed = end - start;
        result.total_time += elapsed;
        if (elapsed < result.min_time) result.min_time = elapsed;
        if (elapsed > result.max_time) result.max_time = elapsed;
    }
    
    result.avg_time = result.total_time / iterations;
    
    /* 打印结果 */
    println("=== Benchmark Result ===");
    print("Name: ");
    println(result.name);
    print("Iterations: ");
    print_dec(result.iterations);
    println("");
    print("Total: ");
    print_dec(result.total_time / 1000000);
    println(" ms");
    print("Average: ");
    print_dec(result.avg_time / 1000);
    println(" us");
    print("Min: ");
    print_dec(result.min_time / 1000);
    println(" us");
    print("Max: ");
    print_dec(result.max_time / 1000);
    println(" us");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：系统调用性能监控
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 系统调用统计 */
struct syscall_stats {
    uint64_t count;
    uint64_t total_time;
    uint64_t errors;
    uint64_t max_time;
};

#define MAX_SYSCALLS    256
static struct syscall_stats syscall_stats_table[MAX_SYSCALLS];

/* 记录系统调用 */
void perf_syscall_record(int syscall_num, uint64_t time, int error) {
    if (syscall_num < 0 || syscall_num >= MAX_SYSCALLS) return;
    
    struct syscall_stats *stats = &syscall_stats_table[syscall_num];
    stats->count++;
    stats->total_time += time;
    if (error) stats->errors++;
    if (time > stats->max_time) stats->max_time = time;
}

/* 获取系统调用统计 */
void perf_get_syscall_stats(int syscall_num, struct syscall_stats *stats) {
    if (syscall_num < 0 || syscall_num >= MAX_SYSCALLS || stats == NULL) return;
    *stats = syscall_stats_table[syscall_num];
}

/* 打印系统调用统计 */
void perf_print_syscall_stats(void) {
    println("=== Syscall Statistics ===");
    
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        if (syscall_stats_table[i].count > 0) {
            print("Syscall ");
            print_dec(i);
            print(": ");
            print_dec(syscall_stats_table[i].count);
            print(" calls, ");
            print_dec(syscall_stats_table[i].total_time / 1000000);
            print(" ms total, ");
            print_dec(syscall_stats_table[i].errors);
            println(" errors");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：内存访问模式分析
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 内存访问统计 */
struct mem_access_pattern {
    uint64_t sequential_accesses;
    uint64_t random_accesses;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t prefetch_hits;
    uint64_t last_addr;
};

static struct mem_access_pattern mem_patterns[MAX_CPUS];

/* 记录内存访问 */
void perf_mem_access(int cpu, uint64_t addr, int is_hit) {
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    
    struct mem_access_pattern *pattern = &mem_patterns[cpu];
    
    /* 检测访问模式 */
    if (addr == pattern->last_addr + 64 || addr == pattern->last_addr - 64) {
        pattern->sequential_accesses++;
    } else {
        pattern->random_accesses++;
    }
    
    pattern->last_addr = addr;
    
    if (is_hit) {
        pattern->cache_hits++;
    } else {
        pattern->cache_misses++;
    }
}

/* 获取内存访问模式 */
void perf_get_mem_pattern(int cpu, struct mem_access_pattern *pattern) {
    if (cpu < 0 || cpu >= MAX_CPUS || pattern == NULL) return;
    *pattern = mem_patterns[cpu];
}

/* 打印内存访问模式 */
void perf_print_mem_patterns(void) {
    println("=== Memory Access Patterns ===");
    
    for (int i = 0; i < smp_cpu_count; i++) {
        struct mem_access_pattern *p = &mem_patterns[i];
        uint64_t total = p->sequential_accesses + p->random_accesses;
        
        if (total == 0) continue;
        
        print("CPU ");
        print_dec(i);
        print(": Seq ");
        print_dec((p->sequential_accesses * 100) / total);
        print("% Rand ");
        print_dec((p->random_accesses * 100) / total);
        print("% Hits ");
        print_dec(p->cache_hits);
        print(" Misses ");
        print_dec(p->cache_misses);
        println("");
    }
}

/* ========== 性能分析器高级增强功能 ========== */

/* === 内核延迟分析 === */

struct latency_hist {
    uint64_t buckets[8];
    uint64_t total_samples;
    uint64_t max_latency;
};

static struct latency_hist sched_lat;
static struct latency_hist irq_lat;
static struct latency_hist io_lat;

/* 计算延迟桶索引 */
static int lat_bucket(uint64_t ns) {
    if (ns < 1000) return 0;
    if (ns < 10000) return 1;
    if (ns < 100000) return 2;
    if (ns < 1000000) return 3;
    if (ns < 10000000) return 4;
    if (ns < 100000000) return 5;
    return 6;
}

/* 记录调度延迟 */
void perf_record_sched_latency(uint64_t ns) {
    int idx = lat_bucket(ns);
    sched_lat.buckets[idx]++;
    sched_lat.total_samples++;
    if (ns > sched_lat.max_latency) sched_lat.max_latency = ns;
}

/* 记录中断延迟 */
void perf_record_irq_latency(uint64_t ns) {
    int idx = lat_bucket(ns);
    irq_lat.buckets[idx]++;
    irq_lat.total_samples++;
    if (ns > irq_lat.max_latency) irq_lat.max_latency = ns;
}

/* 记录 I/O 延迟 */
void perf_record_io_latency(uint64_t ns) {
    int idx = lat_bucket(ns);
    io_lat.buckets[idx]++;
    io_lat.total_samples++;
    if (ns > io_lat.max_latency) io_lat.max_latency = ns;
}

/* 打印延迟直方图 */
void perf_print_latency_hist(void) {
    const char *labels[] = {"<1us", "1-10us", "10-100us", "100us-1ms", "1-10ms", "10-100ms", ">100ms"};
    
    println("=== Latency Histograms ===");
    
    println("Scheduling:");
    for (int i = 0; i < 7; i++) {
        print("  "); print(labels[i]); print(": "); print_dec(sched_lat.buckets[i]); println("");
    }
    
    println("IRQ:");
    for (int i = 0; i < 7; i++) {
        print("  "); print(labels[i]); print(": "); print_dec(irq_lat.buckets[i]); println("");
    }
    
    println("I/O:");
    for (int i = 0; i < 7; i++) {
        print("  "); print(labels[i]); print(": "); print_dec(io_lat.buckets[i]); println("");
    }
}

/* === CPU 火焰图采样 === */

#define FLAME_MAX_ENTRIES   512
#define FLAME_MAX_DEPTH     32

struct flame_entry {
    uint64_t stack[FLAME_MAX_DEPTH];
    uint32_t depth;
    uint32_t count;
    int valid;
};

static struct flame_entry flame_entries[FLAME_MAX_ENTRIES];
static int flame_enabled = 0;
static volatile int flame_lock = 0;

/* 启用火焰图 */
void perf_flame_enable(int enable) {
    flame_enabled = enable;
}

/* 采集调用栈 */
void perf_flame_sample(uint64_t *stack, uint32_t depth) {
    if (!flame_enabled || depth == 0 || depth > FLAME_MAX_DEPTH) return;
    
    while (__sync_lock_test_and_set(&flame_lock, 1)) asm volatile("pause");
    
    for (int i = 0; i < FLAME_MAX_ENTRIES; i++) {
        if (!flame_entries[i].valid) {
            flame_entries[i].valid = 1;
            flame_entries[i].depth = depth;
            flame_entries[i].count = 1;
            for (uint32_t j = 0; j < depth; j++) {
                flame_entries[i].stack[j] = stack[j];
            }
            break;
        }
        
        if (flame_entries[i].depth == depth) {
            int match = 1;
            for (uint32_t j = 0; j < depth; j++) {
                if (flame_entries[i].stack[j] != stack[j]) { match = 0; break; }
            }
            if (match) {
                flame_entries[i].count++;
                break;
            }
        }
    }
    
    __sync_lock_release(&flame_lock);
}

/* 打印火焰图数据 */
void perf_flame_print(void) {
    println("=== Flamegraph Samples ===");
    for (int i = 0; i < FLAME_MAX_ENTRIES; i++) {
        if (flame_entries[i].valid) {
            print("Sample "); print_dec(i); print(" count="); print_dec(flame_entries[i].count); println("");
        }
    }
}

/* === 性能预警 === */

#define PERF_WARN_MAX   16

struct perf_warn {
    char msg[64];
    uint64_t time;
    int severity;
};

static struct perf_warn perf_warns[PERF_WARN_MAX];
static int perf_warn_idx = 0;

/* 添加性能警告 */
void perf_add_warn(const char *msg, int severity) {
    struct perf_warn *w = &perf_warns[perf_warn_idx];
    strncpy(w->msg, msg, sizeof(w->msg) - 1);
    w->msg[sizeof(w->msg) - 1] = '\0';
    w->time = get_tick_count();
    w->severity = severity;
    perf_warn_idx = (perf_warn_idx + 1) % PERF_WARN_MAX;
}

/* 检查性能阈值 */
void perf_check_thresholds(void) {
    if (sched_lat.max_latency > 10000000ULL) {
        perf_add_warn("High scheduling latency", 2);
    }
    if (irq_lat.max_latency > 10000000ULL) {
        perf_add_warn("High IRQ latency", 2);
    }
}

/* 打印性能警告 */
void perf_print_warns(void) {
    println("=== Performance Warnings ===");
    for (int i = 0; i < PERF_WARN_MAX; i++) {
        if (perf_warns[i].msg[0]) {
            print("["); print_dec(perf_warns[i].severity); print("] ");
            println(perf_warns[i].msg);
        }
    }
}

/* === 性能报告 === */

void perf_generate_report(void) {
    println("╔══════════════════════════════════════════╗");
    println("║       Kernel Performance Report          ║");
    println("╚══════════════════════════════════════════╝");

    perf_print_latency_hist();
    println("");
    perf_print_warns();
    println("");
    perf_print_hotspots();
    println("");
    perf_print_mem_patterns();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PMU 事件采样 (Performance Monitoring Unit)
 * 使用 CPU PMU 硬件对 CPU 事件进行采样
 * ═══════════════════════════════════════════════════════════════════════════ */

/* PMU 采样状态 */
struct pmu_state {
    int initialized;
    int sampling_active;
    struct pmu_sample_config config;
    struct pmu_sample_record *sample_buffer;
    uint32_t sample_count;
    uint32_t sample_capacity;
    uint64_t total_samples;
    uint64_t total_period;
    uint64_t lost_samples;
    spinlock_t lock;
};

static struct pmu_state pmu_state;

/* PMU 事件到 MSR 选择器映射 (Intel x86_64) */
static const uint32_t pmu_event_sel[PMU_EVENT_COUNT] = {
    [PMU_EVENT_CPU_CYCLES]       = 0x003C,   /* CPU_CLK_UNHALTED.THREAD */
    [PMU_EVENT_INSTRUCTIONS]     = 0x00C0,   /* INST_RETIRED.ANY */
    [PMU_EVENT_CACHE_REFILLS]    = 0x004F,   /* L1D.REPLACEMENT */
    [PMU_EVENT_CACHE_MISSES]     = 0x012E,   /* L1D.REPL */
    [PMU_EVENT_BRANCH_MISPREDICT]= 0x00C5,   /* BR_MISP_RETIRED.ALL_BRANCHES */
    [PMU_EVENT_BUS_ACCESS]       = 0x0140,   /* BUS_TRANS_MEM */
    [PMU_EVENT_L1D_CACHE_REFILL] = 0x0051,   /* L1D.REPL */
    [PMU_EVENT_L1I_CACHE_REFILL] = 0x0028,   /* L1I.REPL */
    [PMU_EVENT_LL_CACHE_REFILL]  = 0x0027,   /* L2_RETIRED.L2_MISS */
};

/* 初始化 PMU 采样 */
int perf_pmu_init(void) {
    memset(&pmu_state, 0, sizeof(pmu_state));
    pmu_state.sample_capacity = 4096;
    pmu_state.sample_buffer = kmalloc(sizeof(struct pmu_sample_record) * pmu_state.sample_capacity);
    if (pmu_state.sample_buffer == NULL) {
        pmu_state.sample_capacity = 0;
        return -1;
    }
    pmu_state.lock = SPINLOCK_INIT;
    pmu_state.initialized = 1;

    println("[PMU] PMU sampling initialized");
    return 0;
}

/* 启动 PMU 采样 */
int perf_pmu_start(struct pmu_sample_config *config) {
    if (config == NULL || !pmu_state.initialized) return -1;

    spin_lock(&pmu_state.lock);

    if (pmu_state.sampling_active) {
        spin_unlock(&pmu_state.lock);
        return -1;  /* 已在采样 */
    }

    pmu_state.config = *config;
    pmu_state.sample_count = 0;
    pmu_state.total_samples = 0;
    pmu_state.total_period = 0;
    pmu_state.lost_samples = 0;

    /* 配置 PMU 寄存器 */
    if (config->event < PMU_EVENT_COUNT) {
        uint32_t sel = pmu_event_sel[config->event];
        /* 设置采样周期 (默认 100000) */
        uint64_t period = config->period > 0 ? config->period : 100000;

        /* 写入 MSR 启用计数器 */
        /* IA32_PERFEVTSEL0: 0x186, IA32_PMC0: 0x0C1 */
        uint64_t evt_sel = ((uint64_t)sel & 0xFF) |        /* Event Select */
                           (((uint64_t)sel >> 8) & 0xFF) << 8 |  /* UMask */
                           (1ULL << 22) |              /* EN (Enable) */
                           (1ULL << 20) |              /* APIC interrupt enable */
                           (config->kernel_only ? (1ULL << 17) : 0) |  /* OS */
                           (config->user_only ? (1ULL << 16) : 0);     /* USR */

        /* 写入 MSR (使用 wrmsr 指令) */
        asm volatile(
            "wrmsr"
            :
            : "c"(0x186U), "a"((uint32_t)evt_sel), "d"((uint32_t)(evt_sel >> 32))
        );

        /* 设置计数器初始值 */
        uint64_t init_val = (uint64_t)(-period);
        asm volatile(
            "wrmsr"
            :
            : "c"(0x0C1U), "a"((uint32_t)init_val), "d"((uint32_t)(init_val >> 32))
        );
    }

    pmu_state.sampling_active = 1;
    spin_unlock(&pmu_state.lock);

    print("[PMU] Sampling started for event ");
    print_dec(config->event);
    println("");
    return 0;
}

/* 停止 PMU 采样 */
int perf_pmu_stop(void) {
    if (!pmu_state.initialized) return -1;

    spin_lock(&pmu_state.lock);
    pmu_state.sampling_active = 0;

    /* 禁用 PMU 计数器 */
    asm volatile(
        "wrmsr"
        :
        : "c"(0x186U), "a"(0U), "d"(0U)
    );

    spin_unlock(&pmu_state.lock);

    println("[PMU] Sampling stopped");
    return 0;
}

/* 读取 PMU 采样记录 */
int perf_pmu_read_samples(struct pmu_sample_record *records, int max_count) {
    if (records == NULL || max_count <= 0) return 0;

    spin_lock(&pmu_state.lock);

    int count = 0;
    int available = pmu_state.sample_count < (uint32_t)max_count ?
                    pmu_state.sample_count : (uint32_t)max_count;

    for (int i = 0; i < available; i++) {
        records[i] = pmu_state.sample_buffer[i];
        count++;
    }

    spin_unlock(&pmu_state.lock);
    return count;
}

/* 获取 PMU 采样统计 */
void perf_pmu_get_stats(uint64_t *total_samples, uint64_t *total_period,
                        uint64_t *lost_samples) {
    if (total_samples) *total_samples = pmu_state.total_samples;
    if (total_period) *total_period = pmu_state.total_period;
    if (lost_samples) *lost_samples = pmu_state.lost_samples;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 火焰图生成 (Flame Graph Generation)
 * 收集栈跟踪并格式化为折叠栈数据，供 flamegraph.pl 可视化
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 生成火焰图折叠栈数据 */
int perf_flame_generate_folded(struct flame_folded_entry *entries,
                                int max_entries, int *actual_count) {
    if (entries == NULL || max_entries <= 0) return -1;

    int entry_count = 0;

    /* 从火焰图采样数据生成折叠栈 */
    spin_lock(&perf_state.lock);

    for (int i = 0; i < FLAME_MAX_ENTRIES && entry_count < max_entries; i++) {
        if (!flame_entries[i].valid) continue;

        /* 构建折叠栈字符串 */
        struct flame_folded_entry *fe = &entries[entry_count];
        int pos = 0;

        /* 从栈底到栈顶构建字符串 */
        for (int j = flame_entries[i].depth - 1; j >= 0; j--) {
            if (pos > 0 && pos < 254) {
                fe->stack_str[pos++] = ';';
            }

            /* 将地址转换为函数名或十六进制字符串 */
            uint64_t addr = flame_entries[i].stack[j];
            if (pos < 240) {
                /* 简化：使用十六进制地址作为函数名 */
                fe->stack_str[pos++] = '0';
                fe->stack_str[pos++] = 'x';
                /* 将地址转为十六进制 */
                for (int k = 60; k >= 0 && pos < 254; k -= 4) {
                    int nibble = (addr >> k) & 0xF;
                    fe->stack_str[pos++] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
                }
            }
        }

        fe->stack_str[pos] = '\0';
        fe->count = flame_entries[i].count;
        entry_count++;
    }

    spin_unlock(&perf_state.lock);

    if (actual_count != NULL) {
        *actual_count = entry_count;
    }

    return 0;
}

/* 输出火焰图数据到文本格式 (folded stack format) */
int perf_flame_output_folded_text(char *buf, size_t buf_size) {
    if (buf == NULL || buf_size == 0) return -1;

    struct flame_folded_entry entries[256];
    int count = 0;

    int ret = perf_flame_generate_folded(entries, 256, &count);
    if (ret != 0) return -1;

    size_t pos = 0;
    for (int i = 0; i < count && pos < buf_size - 1; i++) {
        /* 写入 "stack_string count\n" */
        const char *s = entries[i].stack_str;
        while (*s && pos < buf_size - 2) {
            buf[pos++] = *s++;
        }
        buf[pos++] = ' ';

        /* 写入计数值 */
        uint64_t val = entries[i].count;
        char num_buf[21];
        int num_len = 0;
        if (val == 0) {
            num_buf[num_len++] = '0';
        } else {
            while (val > 0 && num_len < 20) {
                num_buf[num_len++] = '0' + (val % 10);
                val /= 10;
            }
        }
        /* 反转数字 */
        for (int j = 0; j < num_len / 2; j++) {
            char tmp = num_buf[j];
            num_buf[j] = num_buf[num_len - 1 - j];
            num_buf[num_len - 1 - j] = tmp;
        }
        for (int j = 0; j < num_len && pos < buf_size - 2; j++) {
            buf[pos++] = num_buf[j];
        }
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';

    return (int)pos;
}

/* 清除火焰图采样数据 */
void perf_flame_clear_data(void) {
    while (__sync_lock_test_and_set(&flame_lock, 1)) asm volatile("pause");
    memset(flame_entries, 0, sizeof(flame_entries));
    __sync_lock_release(&flame_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ftrace 函数追踪 (Function Tracing)
 * 在函数入口插入 NOP/call 指令实现追踪
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ftrace 状态 */
struct ftrace_state {
    int initialized;
    struct ftrace_point points[FTRACE_MAX_POINTS];
    uint32_t point_count;
    struct ftrace_record buffer[FTRACE_BUFFER_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint64_t total_calls;
    uint64_t total_time;
    uint32_t active_points;
    spinlock_t lock;
};

static struct ftrace_state ftrace_state;

/* 初始化 ftrace */
int perf_ftrace_init(void) {
    memset(&ftrace_state, 0, sizeof(ftrace_state));
    ftrace_state.lock = SPINLOCK_INIT;
    ftrace_state.initialized = 1;

    println("[FTRACE] Function tracing initialized");
    return 0;
}

/* 注册 ftrace 追踪点 */
int perf_ftrace_register(uint64_t func_addr, const char *name) {
    if (func_addr == 0 || !ftrace_state.initialized) return -1;

    spin_lock(&ftrace_state.lock);

    if (ftrace_state.point_count >= FTRACE_MAX_POINTS) {
        spin_unlock(&ftrace_state.lock);
        return -1;
    }

    /* 检查是否已注册 */
    for (uint32_t i = 0; i < ftrace_state.point_count; i++) {
        if (ftrace_state.points[i].addr == func_addr) {
            spin_unlock(&ftrace_state.lock);
            return 0;  /* 已注册 */
        }
    }

    /* 注册新追踪点 */
    struct ftrace_point *fp = &ftrace_state.points[ftrace_state.point_count];
    fp->addr = func_addr;
    fp->enabled = 0;
    fp->original_size = 5;  /* NOP 指令占 5 字节 */

    /* 保存原始指令 */
    memcpy(fp->original_code, (void *)func_addr, 16);

    if (name != NULL) {
        int i = 0;
        while (name[i] && i < 63) {
            fp->name[i] = name[i];
            i++;
        }
        fp->name[i] = '\0';
    }

    ftrace_state.point_count++;
    spin_unlock(&ftrace_state.lock);

    return 0;
}

/* 启用 ftrace 追踪点 (在函数入口插入 CALL 指令) */
int perf_ftrace_enable(uint64_t func_addr) {
    if (func_addr == 0 || !ftrace_state.initialized) return -1;

    spin_lock(&ftrace_state.lock);

    for (uint32_t i = 0; i < ftrace_state.point_count; i++) {
        if (ftrace_state.points[i].addr == func_addr) {
            struct ftrace_point *fp = &ftrace_state.points[i];

            if (fp->enabled) {
                spin_unlock(&ftrace_state.lock);
                return 0;  /* 已启用 */
            }

            /* 在函数入口插入 CALL perf_ftrace_caller */
            /* 使用 5 字节相对 CALL: E8 rel32 */
            int64_t rel_offset = (int64_t)(uint64_t)perf_ftrace_caller -
                                  (int64_t)(func_addr + 5);
            if (rel_offset >= -2147483648LL && rel_offset <= 2147483647LL) {
                uint8_t call_code[5];
                call_code[0] = 0xE8;  /* CALL rel32 */
                *(int32_t *)&call_code[1] = (int32_t)rel_offset;
                memcpy((void *)func_addr, call_code, 5);
            }
            /* 如果偏移超出范围，使用间接调用 (简化：跳过) */

            fp->enabled = 1;
            ftrace_state.active_points++;
            spin_unlock(&ftrace_state.lock);
            return 0;
        }
    }

    spin_unlock(&ftrace_state.lock);
    return -1;  /* 未找到 */
}

/* 禁用 ftrace 追踪点 (恢复 NOP 指令) */
int perf_ftrace_disable(uint64_t func_addr) {
    if (func_addr == 0 || !ftrace_state.initialized) return -1;

    spin_lock(&ftrace_state.lock);

    for (uint32_t i = 0; i < ftrace_state.point_count; i++) {
        if (ftrace_state.points[i].addr == func_addr) {
            struct ftrace_point *fp = &ftrace_state.points[i];

            if (!fp->enabled) {
                spin_unlock(&ftrace_state.lock);
                return 0;
            }

            /* 恢复原始指令 (NOP: 0F 1F 44 00 00) */
            uint8_t nop5[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
            memcpy((void *)func_addr, nop5, 5);

            fp->enabled = 0;
            ftrace_state.active_points--;
            spin_unlock(&ftrace_state.lock);
            return 0;
        }
    }

    spin_unlock(&ftrace_state.lock);
    return -1;
}

/* 启用所有追踪点 */
int perf_ftrace_enable_all(void) {
    int enabled = 0;

    spin_lock(&ftrace_state.lock);
    for (uint32_t i = 0; i < ftrace_state.point_count; i++) {
        if (!ftrace_state.points[i].enabled) {
            spin_unlock(&ftrace_state.lock);
            perf_ftrace_enable(ftrace_state.points[i].addr);
            spin_lock(&ftrace_state.lock);
            enabled++;
        }
    }
    spin_unlock(&ftrace_state.lock);

    return enabled;
}

/* 禁用所有追踪点 */
int perf_ftrace_disable_all(void) {
    int disabled = 0;

    spin_lock(&ftrace_state.lock);
    for (uint32_t i = 0; i < ftrace_state.point_count; i++) {
        if (ftrace_state.points[i].enabled) {
            spin_unlock(&ftrace_state.lock);
            perf_ftrace_disable(ftrace_state.points[i].addr);
            spin_lock(&ftrace_state.lock);
            disabled++;
        }
    }
    spin_unlock(&ftrace_state.lock);

    return disabled;
}

/* 读取 ftrace 记录 */
int perf_ftrace_read(struct ftrace_record *records, int max_count) {
    if (records == NULL || max_count <= 0) return 0;

    spin_lock(&ftrace_state.lock);

    int count = 0;
    while (ftrace_state.tail != ftrace_state.head && count < max_count) {
        records[count++] = ftrace_state.buffer[ftrace_state.tail];
        ftrace_state.tail = (ftrace_state.tail + 1) % FTRACE_BUFFER_SIZE;
        ftrace_state.count--;
    }

    spin_unlock(&ftrace_state.lock);
    return count;
}

/* ftrace 入口回调 (由被追踪函数调用) */
void perf_ftrace_caller(uint64_t ip, uint64_t parent_ip) {
    spin_lock(&ftrace_state.lock);

    /* 记录追踪信息 */
    if (ftrace_state.count < FTRACE_BUFFER_SIZE) {
        struct ftrace_record *rec = &ftrace_state.buffer[ftrace_state.head];
        rec->ip = ip;
        rec->parent_ip = parent_ip;
        rec->timestamp = get_tick_count();
        rec->cpu = smp_processor_id();
        rec->flags = 0;

        ftrace_state.head = (ftrace_state.head + 1) % FTRACE_BUFFER_SIZE;
        ftrace_state.count++;
    }

    ftrace_state.total_calls++;

    spin_unlock(&ftrace_state.lock);
}

/* 获取 ftrace 统计 */
void perf_ftrace_get_stats(uint64_t *total_calls, uint64_t *total_time,
                           uint32_t *active_points) {
    if (total_calls) *total_calls = ftrace_state.total_calls;
    if (total_time) *total_time = ftrace_state.total_time;
    if (active_points) *active_points = ftrace_state.active_points;
}

