/*
 * Zratro OS - Unified Performance Analysis Extension (统一性能分析扩展)
 * 
 * 实现所有子系统的统一性能分析
 */

#include "perf_analysis.h"
#include "perf.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "pmm.h"
#include "scheduler.h"

/* snprintf 替代实现 */
static int pa_snprintf(char *buf, size_t size, const char *fmt, ...) {
    if (buf == NULL || size == 0) return 0;
    
    /* 简化实现：只处理纯字符串 */
    size_t len = 0;
    const char *p = fmt;
    while (*p && len < size - 1) {
        if (*p == '%' && *(p + 1) == 's') {
            /* %s 格式 */
            p += 2;
            // 简化：跳过 %s 处理
        } else if (*p == '%' && *(p + 1) == 'd') {
            /* %d 格式 - 使用 print_dec 风格 */
            p += 2;
            // 简化：跳过 %d 处理
        } else if (*p == '%' && *(p + 1) == 'l' && *(p + 2) == 'u') {
            /* %lu 格式 */
            p += 3;
            // 简化：跳过 %lu 处理
        } else if (*p == '%' && *(p + 1) == 'l' && *(p + 2) == 'l' && *(p + 3) == 'u') {
            /* %llu 格式 */
            p += 4;
            // 简化：跳过 %llu 处理
        } else {
            buf[len++] = *p++;
        }
    }
    buf[len] = '\0';
    return (int)len;
}

/* ========== 全局管理器 ========== */

static struct pa_manager global_pa;

/* ========== 初始化/销毁 ========== */

int pa_init(struct pa_manager *pa) {
    if (pa == NULL) {
        return -1;
    }
    
    memset(pa, 0, sizeof(*pa));
    
    INIT_LIST_HEAD(&pa->subsystems);
    
    pa->sample_interval = 1000000;  /* 1ms */
    pa->start_time = get_tick_count();
    pa->last_update = pa->start_time;
    
    spinlock_init(&pa->lock);
    
    return 0;
}

void pa_destroy(struct pa_manager *pa) {
    if (pa == NULL) {
        return;
    }
    
    spin_lock(&pa->lock);
    
    /* 释放所有子系统 */
    struct pa_subsys_stats *ss, *ss_tmp;
    list_for_each_entry_safe(ss, ss_tmp, &pa->subsystems, list) {
        /* 释放指标 */
        struct pa_metric *m, *m_tmp;
        list_for_each_entry_safe(m, m_tmp, &ss->metrics, list) {
            list_del(&m->list);
            kfree(m);
        }
        
        list_del(&ss->list);
        kfree(ss);
    }
    
    /* 释放火焰图 */
    if (pa->flame_graph.root != NULL) {
        kfree(pa->flame_graph.root);
    }
    
    spin_unlock(&pa->lock);
}

/* ========== 子系统注册 ========== */

struct pa_subsys_stats *pa_register_subsystem(struct pa_manager *pa,
                                               pa_subsys_id_t id,
                                               const char *name) {
    if (pa == NULL || name == NULL) {
        return NULL;
    }
    
    spin_lock(&pa->lock);
    
    /* 检查是否已注册 */
    struct pa_subsys_stats *ss;
    list_for_each_entry(ss, &pa->subsystems, list) {
        if (ss->id == id) {
            spin_unlock(&pa->lock);
            return ss;
        }
    }
    
    /* 创建新子系统 */
    ss = kmalloc(sizeof(*ss));
    if (ss == NULL) {
        spin_unlock(&pa->lock);
        return NULL;
    }
    
    memset(ss, 0, sizeof(*ss));
    ss->id = id;
    strncpy(ss->name, name, PA_MAX_NAME_LEN - 1);
    INIT_LIST_HEAD(&ss->metrics);
    INIT_LIST_HEAD(&ss->list);
    
    list_add_tail(&ss->list, &pa->subsystems);
    pa->subsystem_count++;
    
    spin_unlock(&pa->lock);
    
    return ss;
}

int pa_unregister_subsystem(struct pa_manager *pa, pa_subsys_id_t id) {
    if (pa == NULL) {
        return -1;
    }
    
    spin_lock(&pa->lock);
    
    struct pa_subsys_stats *ss, *tmp;
    list_for_each_entry_safe(ss, tmp, &pa->subsystems, list) {
        if (ss->id == id) {
            list_del(&ss->list);
            kfree(ss);
            pa->subsystem_count--;
            spin_unlock(&pa->lock);
            return 0;
        }
    }
    
    spin_unlock(&pa->lock);
    return -1;
}

/* ========== 指标管理 ========== */

struct pa_metric *pa_register_metric(struct pa_manager *pa,
                                      pa_subsys_id_t subsystem,
                                      const char *name,
                                      pa_metric_type_t type) {
    if (pa == NULL || name == NULL) {
        return NULL;
    }
    
    /* 查找子系统 */
    struct pa_subsys_stats *ss = NULL;
    spin_lock(&pa->lock);
    list_for_each_entry(ss, &pa->subsystems, list) {
        if (ss->id == subsystem) {
            break;
        }
    }
    spin_unlock(&pa->lock);
    
    if (ss == NULL) {
        return NULL;
    }
    
    struct pa_metric *metric = kmalloc(sizeof(*metric));
    if (metric == NULL) {
        return NULL;
    }
    
    memset(metric, 0, sizeof(*metric));
    metric->subsystem = subsystem;
    metric->type = type;
    metric->flags = PA_METRIC_FLAG_ENABLED;
    strncpy(metric->name, name, PA_MAX_NAME_LEN - 1);
    INIT_LIST_HEAD(&metric->list);
    
    spin_lock(&pa->lock);
    metric->metric_id = ++ss->metric_count;
    list_add_tail(&metric->list, &ss->metrics);
    spin_unlock(&pa->lock);
    
    return metric;
}

int pa_update_metric(struct pa_manager *pa, uint32_t metric_id, uint64_t value) {
    if (pa == NULL) {
        return -1;
    }
    
    spin_lock(&pa->lock);
    
    struct pa_subsys_stats *ss;
    list_for_each_entry(ss, &pa->subsystems, list) {
        struct pa_metric *m;
        list_for_each_entry(m, &ss->metrics, list) {
            if (m->metric_id == metric_id) {
                m->last_value = m->value;
                m->value = value;
                m->update_time = get_tick_count();
                spin_unlock(&pa->lock);
                return 0;
            }
        }
    }
    
    spin_unlock(&pa->lock);
    return -1;
}

int pa_increment_metric(struct pa_manager *pa, uint32_t metric_id, uint64_t delta) {
    if (pa == NULL) {
        return -1;
    }
    
    spin_lock(&pa->lock);
    
    struct pa_subsys_stats *ss;
    list_for_each_entry(ss, &pa->subsystems, list) {
        struct pa_metric *m;
        list_for_each_entry(m, &ss->metrics, list) {
            if (m->metric_id == metric_id) {
                m->value += delta;
                m->update_time = get_tick_count();
                spin_unlock(&pa->lock);
                return 0;
            }
        }
    }
    
    spin_unlock(&pa->lock);
    return -1;
}

int pa_record_sample(struct pa_manager *pa, uint32_t metric_id, uint64_t value) {
    if (pa == NULL) {
        return -1;
    }
    
    spin_lock(&pa->lock);
    
    struct pa_subsys_stats *ss;
    list_for_each_entry(ss, &pa->subsystems, list) {
        struct pa_metric *m;
        list_for_each_entry(m, &ss->metrics, list) {
            if (m->metric_id == metric_id) {
                m->sample_count++;
                m->sample_sum += value;
                
                /* 更新直方图 */
                if (value < m->histogram.min || m->histogram.count == 0) {
                    m->histogram.min = value;
                }
                if (value > m->histogram.max) {
                    m->histogram.max = value;
                }
                m->histogram.sum += value;
                m->histogram.count++;
                
                /* 放入桶中 */
                for (int i = 0; i < PA_MAX_LATENCY_BUCKETS; i++) {
                    if (value <= m->histogram.bucket_bounds[i] || 
                        m->histogram.bucket_bounds[i] == 0) {
                        m->histogram.buckets[i]++;
                        break;
                    }
                }
                
                spin_unlock(&pa->lock);
                return 0;
            }
        }
    }
    
    spin_unlock(&pa->lock);
    return -1;
}

/* ========== 统计更新 ========== */

int pa_update_cpu_stats(struct pa_manager *pa, int cpu) {
    if (pa == NULL || cpu < 0 || cpu >= 8) {
        return -1;
    }
    
    struct pa_cpu_stats *cs = &pa->cpu_stats[cpu];
    cs->cpu_id = cpu;
    
    /* 从现有 perf 系统获取数据 */
    cs->context_switches = perf_read(PERF_SCHED_CONTEXT_SWITCHES);
    cs->preemptions = perf_read(PERF_SCHED_PREEMPTIONS);
    
    return 0;
}

int pa_update_memory_stats(struct pa_manager *pa) {
    if (pa == NULL) {
        return -1;
    }
    
    struct pa_memory_stats *ms = &pa->memory_stats;
    
    /* 从 PMM 获取数据 */
    ms->total = pmm_get_total_memory();
    ms->free = pmm_get_free_memory();
    ms->used = ms->total - ms->free;
    
    /* 从 perf 获取页错误 */
    ms->page_faults = perf_read(PERF_MEM_PAGE_FAULTS);
    ms->cache_hits = perf_read(PERF_MEM_CACHE_HITS);
    ms->cache_misses = perf_read(PERF_MEM_CACHE_MISSES);
    
    /* 计算缓存命中率 */
    if (ms->cache_hits + ms->cache_misses > 0) {
        ms->cache_hit_rate = (ms->cache_hits * 1000) / (ms->cache_hits + ms->cache_misses);
    }
    
    return 0;
}

int pa_update_io_stats(struct pa_manager *pa) {
    if (pa == NULL) {
        return -1;
    }
    
    struct pa_io_stats *io = &pa->io_stats;
    
    io->read_ops = perf_read(PERF_FS_READS);
    io->write_ops = perf_read(PERF_FS_WRITES);
    io->read_bytes = perf_read(PERF_IO_BYTES);
    
    return 0;
}

int pa_update_network_stats(struct pa_manager *pa) {
    if (pa == NULL) {
        return -1;
    }
    
    struct pa_network_stats *ns = &pa->network_stats;
    
    ns->packets_in = perf_read(PERF_NET_PACKETS_IN);
    ns->packets_out = perf_read(PERF_NET_PACKETS_OUT);
    ns->bytes_in = perf_read(PERF_NET_BYTES_IN);
    ns->bytes_out = perf_read(PERF_NET_BYTES_OUT);
    ns->tcp_retransmits = perf_read(PERF_NET_TCP_RETRANSMITS);
    
    return 0;
}

int pa_update_scheduler_stats(struct pa_manager *pa) {
    if (pa == NULL) {
        return -1;
    }
    
    struct pa_scheduler_stats *ss = &pa->scheduler_stats;
    
    ss->context_switches = perf_read(PERF_SCHED_CONTEXT_SWITCHES);
    ss->load_balance_count = perf_read(PERF_SCHED_LOAD_BALANCE);
    
    return 0;
}

/* ========== 汇总接口 ========== */

int pa_collect_all(struct pa_manager *pa) {
    if (pa == NULL) {
        return -1;
    }
    
    for (uint32_t i = 0; i < pa->cpu_count; i++) {
        pa_update_cpu_stats(pa, i);
    }
    
    pa_update_memory_stats(pa);
    pa_update_io_stats(pa);
    pa_update_network_stats(pa);
    pa_update_scheduler_stats(pa);
    
    pa->last_update = get_tick_count();
    
    return 0;
}

uint64_t pa_calc_utilization(struct pa_manager *pa, pa_subsys_id_t subsystem) {
    if (pa == NULL) {
        return 0;
    }
    
    spin_lock(&pa->lock);
    
    struct pa_subsys_stats *ss;
    list_for_each_entry(ss, &pa->subsystems, list) {
        if (ss->id == subsystem) {
            uint64_t total = ss->busy_time + ss->idle_time;
            if (total == 0) {
                spin_unlock(&pa->lock);
                return 0;
            }
            uint64_t util = (ss->busy_time * 1000) / total;
            spin_unlock(&pa->lock);
            return util;
        }
    }
    
    spin_unlock(&pa->lock);
    return 0;
}

uint64_t pa_calc_throughput(struct pa_manager *pa, pa_subsys_id_t subsystem) {
    if (pa == NULL) {
        return 0;
    }
    
    uint64_t elapsed = get_tick_count() - pa->start_time;
    if (elapsed == 0) {
        return 0;
    }
    
    spin_lock(&pa->lock);
    
    struct pa_subsys_stats *ss;
    list_for_each_entry(ss, &pa->subsystems, list) {
        if (ss->id == subsystem) {
            /* ops per second */
            uint64_t throughput = (ss->total_operations * 1000000) / elapsed;
            spin_unlock(&pa->lock);
            return throughput;
        }
    }
    
    spin_unlock(&pa->lock);
    return 0;
}

uint64_t pa_calc_latency_percentile(struct pa_manager *pa,
                                     pa_subsys_id_t subsystem,
                                     uint32_t percentile) {
    (void)pa;
    (void)subsystem;
    (void)percentile;
    
    /* 简化实现 */
    return 0;
}

/* ========== 火焰图 ========== */

int pa_start_profiling(struct pa_manager *pa, uint64_t interval) {
    if (pa == NULL) {
        return -1;
    }
    
    pa->profiling_enabled = 1;
    pa->sample_interval = interval;
    pa->sample_count = 0;
    
    /* 初始化火焰图 */
    if (pa->flame_graph.root == NULL) {
        pa->flame_graph.root = kmalloc(sizeof(struct pa_flame_node));
        if (pa->flame_graph.root != NULL) {
            memset(pa->flame_graph.root, 0, sizeof(struct pa_flame_node));
            pa->flame_graph.depth = 0;
            pa->flame_graph.node_count = 1;
        }
    }
    
    return 0;
}

int pa_stop_profiling(struct pa_manager *pa) {
    if (pa == NULL) {
        return -1;
    }
    
    pa->profiling_enabled = 0;
    
    return 0;
}

int pa_take_sample(struct pa_manager *pa) {
    if (pa == NULL || !pa->profiling_enabled) {
        return -1;
    }
    
    pa->sample_count++;
    pa->flame_graph.total_samples++;
    
    /* TODO: 实际采样调用栈 */
    
    return 0;
}

struct pa_flame_graph *pa_get_flame_graph(struct pa_manager *pa) {
    if (pa == NULL) {
        return NULL;
    }
    return &pa->flame_graph;
}

/* ========== 报告生成 ========== */

int pa_generate_report(struct pa_manager *pa, char *buf, size_t size) {
    if (pa == NULL || buf == NULL || size == 0) {
        return -1;
    }
    
    int pos = 0;
    
    pos += pa_snprintf(buf + pos, size - pos, "=== Performance Report ===\n");
    
    /* 内存 */
    pos += pa_snprintf(buf + pos, size - pos, 
                    "Memory: %lu/%lu MB (%lu%% used)\n",
                    pa->memory_stats.used / (1024 * 1024),
                    pa->memory_stats.total / (1024 * 1024),
                    pa->memory_stats.total > 0 ? 
                    (pa->memory_stats.used * 100) / pa->memory_stats.total : 0);
    
    /* 网络 */
    pos += pa_snprintf(buf + pos, size - pos,
                    "Network: %lu packets in, %lu out\n",
                    pa->network_stats.packets_in,
                    pa->network_stats.packets_out);
    
    /* 调度器 */
    pos += pa_snprintf(buf + pos, size - pos,
                    "Scheduler: %lu context switches\n",
                    pa->scheduler_stats.context_switches);
    
    /* 子系统 */
    struct pa_subsys_stats *ss;
    list_for_each_entry(ss, &pa->subsystems, list) {
        pos += pa_snprintf(buf + pos, size - pos,
                        "  [%s] ops=%lu, errors=%lu\n",
                        ss->name, ss->total_operations, ss->total_errors);
    }
    
    return pos;
}

void pa_print_summary(struct pa_manager *pa) {
    if (pa == NULL) {
        return;
    }
    
    println("=== Performance Summary ===");
    
    print("  Memory: ");
    print_dec(pa->memory_stats.used / (1024 * 1024));
    print("/");
    print_dec(pa->memory_stats.total / (1024 * 1024));
    println(" MB");
    
    print("  Cache Hit Rate: ");
    print_dec(pa->memory_stats.cache_hit_rate / 10);
    print(".");
    print_dec(pa->memory_stats.cache_hit_rate % 10);
    println("%");
    
    print("  Context Switches: ");
    print_dec(pa->scheduler_stats.context_switches);
    println("");
    
    print("  Network I/O: ");
    print_dec(pa->network_stats.bytes_in / 1024);
    print("/");
    print_dec(pa->network_stats.bytes_out / 1024);
    println(" KB");
}

void pa_print_subsystem(struct pa_manager *pa, pa_subsys_id_t id) {
    if (pa == NULL) {
        return;
    }
    
    spin_lock(&pa->lock);
    
    struct pa_subsys_stats *ss;
    list_for_each_entry(ss, &pa->subsystems, list) {
        if (ss->id == id) {
            print("=== Subsystem: ");
            print(ss->name);
            println(" ===");
            
            print("  Operations: ");
            print_dec(ss->total_operations);
            println("");
            
            print("  Errors: ");
            print_dec(ss->total_errors);
            println("");
            
            print("  Metrics: ");
            print_dec(ss->metric_count);
            println("");
            
            break;
        }
    }
    
    spin_unlock(&pa->lock);
}

/* ========== 全局管理器 ========== */

struct pa_manager *pa_get_global(void) {
    return &global_pa;
}

int pa_global_init(void) {
    int ret = pa_init(&global_pa);
    
    if (ret == 0) {
        /* 注册所有子系统 */
        pa_register_subsystem(&global_pa, PA_SUBSYS_SCHEDULER, "scheduler");
        pa_register_subsystem(&global_pa, PA_SUBSYS_MEMORY, "memory");
        pa_register_subsystem(&global_pa, PA_SUBSYS_FS, "filesystem");
        pa_register_subsystem(&global_pa, PA_SUBSYS_NETWORK, "network");
        pa_register_subsystem(&global_pa, PA_SUBSYS_PMEM, "pmem");
        pa_register_subsystem(&global_pa, PA_SUBSYS_ZPIPE, "zpipe");
        pa_register_subsystem(&global_pa, PA_SUBSYS_CAPABILITY, "capability");
        pa_register_subsystem(&global_pa, PA_SUBSYS_SELFHEAL, "selfheal");
        pa_register_subsystem(&global_pa, PA_SUBSYS_COROUTINE, "coroutine");
        pa_register_subsystem(&global_pa, PA_SUBSYS_HETEROG, "heterog");
        pa_register_subsystem(&global_pa, PA_SUBSYS_REALTIME, "realtime");
        pa_register_subsystem(&global_pa, PA_SUBSYS_NETZC, "net_zc");
        pa_register_subsystem(&global_pa, PA_SUBSYS_UTX, "utx");
        
        global_pa.cpu_count = 1;  /* TODO: 获取实际 CPU 数 */
    }
    
    return ret;
}
