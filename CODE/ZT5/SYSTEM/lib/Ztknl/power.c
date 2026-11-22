/*
 * Zratro OS - Power Management Implementation
 * 
 * ACPI 电源管理和 CPU 省电功能
 */

#include "power.h"
#include "console.h"
#include "smp.h"
#include "apic.h"
#include "heap.h"
#include "string.h"

/* I/O 端口辅助函数 - 放在最前面 */
static inline void outb_impl(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb_impl(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* 如果 inb/outb 未定义，使用我们的实现 */
#ifndef inb
#define inb inb_impl
#define outb outb_impl
#endif

/* ACPI FADT 表 */
struct acpi_fadt {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    char creator_id[4];
    uint32_t creator_revision;
    
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t reserved2;
    uint16_t plvl2_lat;
    uint16_t plvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved3[2];
    uint32_t flags;
} __attribute__((packed));

/* 电源状态 */
static int acpi_enabled = 0;
static uint32_t pm1a_cnt = 0;
static uint32_t pm1b_cnt = 0;
static uint16_t sci_irq = 0;

/* 前向声明 */
static void init_pstates(void);
static void init_cstates(void);
static void init_tstates(void);

/* CPU P-State (性能状态) */
struct cpu_pstate {
    uint32_t frequency;     /* 频率 MHz */
    uint32_t voltage;       /* 电压 mV */
    uint32_t latency;       /* 切换延迟 us */
    uint8_t  transition;    /* 过渡值 */
    uint8_t  control;       /* 控制值 */
    uint8_t  status;        /* 状态值 */
};

#define MAX_PSTATES     16
static struct cpu_pstate pstates[MAX_PSTATES];
static int pstate_count = 0;
static int current_pstate = 0;

/* CPU C-State (空闲状态) */
struct cpu_cstate {
    uint8_t  type;          /* C-State 类型 (C1, C2, C3) */
    uint16_t latency;       /* 进入/退出延迟 */
    uint32_t power;         /* 功耗 mW */
    uint32_t addr;          /* 寄存器地址 */
    uint8_t  space_id;      /* 地址空间 ID */
    uint8_t  bit_width;     /* 位宽 */
};

#define MAX_CSTATES     8
static struct cpu_cstate cstates[MAX_CSTATES];
static int cstate_count = 0;

/* CPU T-State (限流状态) */
struct cpu_tstate {
    uint8_t  duty_cycle;    /* 占空比 (0-100%) */
    uint8_t  control;       /* 控制值 */
};

#define MAX_TSTATES     8
static struct cpu_tstate tstates[MAX_TSTATES];
static int tstate_count = 0;

/* 电源统计 */
static uint64_t time_in_c0 = 0;     /* 活动时间 */
static uint64_t time_in_c1 = 0;     /* C1 空闲时间 */
static uint64_t time_in_c2 = 0;     /* C2 空闲时间 */
static uint64_t time_in_c3 = 0;     /* C3 空闲时间 */
static uint64_t cstate_transitions = 0;

/* ========== ACPI 基础功能 ========== */

/* 初始化 ACPI 电源管理 */
int power_init(void) {
    println("[POWER] Initializing power management...");
    
    /* 查找 FADT 表 (简化实现) */
    /* 实际实现需要遍历 RSDT/XSDT */
    
    /* 检查 CPU 省电功能 */
    check_cpu_power_features();
    
    /* 初始化 P-State, C-State, T-State */
    init_pstates();
    init_cstates();
    init_tstates();
    
    println("[POWER] Initialized successfully");
    return 0;
}

/* 检查 CPU 电源特性 */
void check_cpu_power_features(void) {
    uint32_t eax, ebx, ecx, edx;
    
    /* CPUID 0x00000005: MWAIT 和 C-State */
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(5));
    
    if (eax & 0x1) {
        println("[POWER] MONITOR/MWAIT supported");
    }
    
    if (ecx & 0x1) {
        print("[POWER] C-State count: ");
        print_dec(ecx >> 24);
        println("");
    }
    
    /* CPUID 0x00000006: 热管理和电源管理 */
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(6));
    
    if (eax & 0x1) {
        println("[POWER] Digital thermometer supported");
    }
    
    if (eax & 0x2) {
        println("[POWER] Dynamic acceleration supported");
    }
    
    if (eax & 0x4) {
        println("[POWER] Always-running APIC timer supported");
    }
}

/* ========== P-State (性能状态) ========== */

/* 初始化 P-State */
static void init_pstates(void) {
    /* 简化实现：假设支持 3 个 P-State */
    pstate_count = 3;
    
    /* P0: 最高性能 */
    pstates[0].frequency = 3000;    /* 3 GHz */
    pstates[0].voltage = 1200;      /* 1.2V */
    pstates[0].latency = 10;        /* 10 us */
    pstates[0].control = 0;
    
    /* P1: 中等性能 */
    pstates[1].frequency = 2000;
    pstates[1].voltage = 1000;
    pstates[1].latency = 10;
    pstates[1].control = 1;
    
    /* P2: 低性能 */
    pstates[2].frequency = 1000;
    pstates[2].voltage = 800;
    pstates[2].latency = 10;
    pstates[2].control = 2;
    
    current_pstate = 0;
}

/* 设置 P-State */
int power_set_pstate(int state) {
    if (state < 0 || state >= pstate_count) {
        return -1;
    }
    
    /* 写入 MSR 或 I/O 端口 */
    /* 简化实现 */
    
    current_pstate = state;
    return 0;
}

/* 获取当前 P-State */
int power_get_pstate(void) {
    return current_pstate;
}

/* 自动调整 P-State (基于负载) */
void power_adjust_pstate(void) {
    uint64_t load = smp_get_system_load();
    
    if (load > 80 && current_pstate > 0) {
        /* 高负载，提升频率 */
        power_set_pstate(current_pstate - 1);
    } else if (load < 30 && current_pstate < pstate_count - 1) {
        /* 低负载，降低频率 */
        power_set_pstate(current_pstate + 1);
    }
}

/* ========== C-State (空闲状态) ========== */

/* 初始化 C-State */
static void init_cstates(void) {
    /* C0: 活动状态 (不需要配置) */
    
    /* C1: HLT 指令 */
    cstates[0].type = 1;
    cstates[0].latency = 1;         /* 1 us */
    cstates[0].power = 1000;        /* ~1W */
    
    /* C2: MWAIT */
    cstates[1].type = 2;
    cstates[1].latency = 50;        /* 50 us */
    cstates[1].power = 100;         /* ~0.1W */
    
    /* C3: 深度睡眠 */
    cstates[2].type = 3;
    cstates[2].latency = 200;       /* 200 us */
    cstates[2].power = 10;          /* ~0.01W */
    
    cstate_count = 3;
}

/* 进入 C-State */
void power_enter_cstate(int state) {
    if (state < 0 || state >= cstate_count) {
        return;
    }
    
    uint64_t start = get_tick_count();
    
    switch (state) {
        case 0:  /* C1: HLT */
            asm volatile("hlt");
            break;
            
        case 1:  /* C2: MWAIT */
            {
                uint32_t hints = 0x10;  /* C2 hint */
                uint32_t ext = 0;
                asm volatile("mfence");
                asm volatile(".byte 0x0f, 0x01, 0xc8"  /* MONITOR */
                             :: "a"(0), "c"(0), "d"(0));
                asm volatile(".byte 0x0f, 0x01, 0xc9"  /* MWAIT */
                             :: "a"(hints), "c"(ext));
            }
            break;
            
        case 2:  /* C3: 深度睡眠 */
            {
                uint32_t hints = 0x20;  /* C3 hint */
                uint32_t ext = 0;
                asm volatile("mfence");
                asm volatile(".byte 0x0f, 0x01, 0xc8"  /* MONITOR */
                             :: "a"(0), "c"(0), "d"(0));
                asm volatile(".byte 0x0f, 0x01, 0xc9"  /* MWAIT */
                             :: "a"(hints), "c"(ext));
            }
            break;
    }
    
    uint64_t end = get_tick_count();
    uint64_t duration = end - start;
    
    /* 更新统计 */
    cstate_transitions++;
    
    switch (state) {
        case 0: time_in_c1 += duration; break;
        case 1: time_in_c2 += duration; break;
        case 2: time_in_c3 += duration; break;
    }
}

/* 选择最佳 C-State */
int power_select_cstate(void) {
    /* 根据预期空闲时间选择 C-State */
    /* 简化实现：默认选择 C1 */
    
    /* 如果预期空闲时间 > 200us，选择 C3 */
    /* 如果预期空闲时间 > 50us，选择 C2 */
    /* 否则选择 C1 */
    
    return 0;  /* C1 */
}

/* 空闲循环 */
void power_idle(void) {
    int state = power_select_cstate();
    power_enter_cstate(state);
}

/* ========== T-State (限流状态) ========== */

/* 初始化 T-State */
static void init_tstates(void) {
    tstate_count = 8;
    
    for (int i = 0; i < tstate_count; i++) {
        tstates[i].duty_cycle = 100 - (i * 12);  /* 100%, 87.5%, 75%, ... */
        tstates[i].control = i;
    }
}

/* 设置 T-State */
int power_set_tstate(int state) {
    if (state < 0 || state >= tstate_count) {
        return -1;
    }
    
    /* 写入 MSR 或 I/O 端口 */
    /* 简化实现 */
    
    return 0;
}

/* ========== 系统睡眠状态 ========== */

/* 进入 S1 (待机) */
int power_enter_s1(void) {
    println("[POWER] Entering S1 sleep state...");
    
    /* 刷新缓存 */
    asm volatile("wbinvd");
    
    /* 写入 PM1 控制寄存器 */
    /* SLP_TYP = S1, SLP_EN = 1 */
    /* 简化实现 */
    
    return 0;
}

/* 进入 S3 (挂起到内存) */
int power_enter_s3(void) {
    println("[POWER] Entering S3 sleep state...");
    
    /* 保存系统状态 */
    /* 简化实现 */
    
    /* 刷新缓存 */
    asm volatile("wbinvd");
    
    /* 写入 PM1 控制寄存器 */
    /* SLP_TYP = S3, SLP_EN = 1 */
    
    return 0;
}

/* 进入 S4 (挂起到磁盘) */
int power_enter_s4(void) {
    println("[POWER] Entering S4 sleep state...");
    
    /* 保存系统状态到磁盘 */
    /* 简化实现 */
    
    return 0;
}

/* 进入 S5 (软关机) */
int power_enter_s5(void) {
    println("[POWER] Entering S5 (shutdown)...");
    
    /* 刷新缓存 */
    asm volatile("wbinvd");
    
    /* 写入 PM1 控制寄存器 */
    /* SLP_TYP = S5, SLP_EN = 1 */
    /* 简化实现 */
    
    /* 无限循环 */
    while (1) {
        asm volatile("hlt");
    }
    
    return 0;
}

/* ========== 系统关机/重启 ========== */

/* 系统关机 */
void power_off(void) {
    power_enter_s5();
}

/* 系统重启 */
void power_reboot(void) {
    println("[POWER] Rebooting system...");
    
    /* 刷新缓存 */
    asm volatile("wbinvd");
    
    /* 键盘控制器重启 */
    /* 等待键盘控制器准备好 */
    while (inb(0x64) & 0x02);
    
    /* 写入重启命令 */
    outb(0x64, 0xFE);
    
    /* 如果失败，使用三重故障 */
    /* 加载空 IDT 并触发中断 */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = { 0, 0 };
    
    asm volatile("lidt %0" :: "m"(null_idt));
    asm volatile("int $3");
    
    /* 无限循环 */
    while (1) {
        asm volatile("hlt");
    }
}

/* ========== 电源统计 ========== */

void power_stats(void) {
    println("=== Power Management Statistics ===");
    
    print("P-State: P");
    print_dec(current_pstate);
    print(" (");
    print_dec(pstates[current_pstate].frequency);
    print(" MHz @ ");
    print_dec(pstates[current_pstate].voltage);
    println(" mV)");
    
    print("C-State transitions: ");
    print_dec(cstate_transitions);
    println("");
    
    print("Time in C0 (active): ");
    print_dec(time_in_c0 / 1000000);
    println(" ms");
    
    print("Time in C1 (halt): ");
    print_dec(time_in_c1 / 1000000);
    println(" ms");
    
    print("Time in C2 (mwait): ");
    print_dec(time_in_c2 / 1000000);
    println(" ms");
    
    print("Time in C3 (deep): ");
    print_dec(time_in_c3 / 1000000);
    println(" ms");
}

/* ========== 电源管理高级增强功能 ========== */

/* === 动态频率调节 (DVFS) === */

struct dvfs_policy {
    uint32_t min_freq;
    uint32_t max_freq;
    uint32_t up_threshold;      /* 负载阈值：向上调频 */
    uint32_t down_threshold;    /* 负载阈值：向下调频 */
    uint64_t sampling_rate;     /* 采样间隔 */
    int governor;               /* 调度策略 */
};

#define DVFS_GOVERNOR_PERFORMANCE  0
#define DVFS_GOVERNOR_POWERSAVE    1
#define DVFS_GOVERNOR_ONDEMAND     2
#define DVFS_GOVERNOR_CONSERVATIVE 3

static struct dvfs_policy dvfs_policies[MAX_CPUS];

/* 初始化 DVFS 策略 */
void dvfs_init_policy(int cpu, int governor) {
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    
    struct dvfs_policy *p = &dvfs_policies[cpu];
    p->min_freq = 800;     /* 最低 800 MHz */
    p->max_freq = 3000;    /* 最高 3000 MHz */
    p->up_threshold = 80;
    p->down_threshold = 20;
    p->sampling_rate = 10000000ULL;  /* 10ms */
    p->governor = governor;
}

/* 更新 DVFS 状态 */
void dvfs_update(int cpu, uint32_t load) {
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    
    struct dvfs_policy *p = &dvfs_policies[cpu];
    uint32_t cur_freq = pstates[current_pstate].frequency;
    
    switch (p->governor) {
        case DVFS_GOVERNOR_ONDEMAND:
            if (load > p->up_threshold && cur_freq < p->max_freq) {
                /* 负载高，提升频率 */
                power_set_pstate(current_pstate > 0 ? current_pstate - 1 : 0);
            } else if (load < p->down_threshold && cur_freq > p->min_freq) {
                /* 负载低，降低频率 */
                power_set_pstate(current_pstate < pstate_count - 1 ? current_pstate + 1 : pstate_count - 1);
            }
            break;
            
        case DVFS_GOVERNOR_PERFORMANCE:
            /* 始终最高频率 */
            power_set_pstate(0);
            break;
            
        case DVFS_GOVERNOR_POWERSAVE:
            /* 始终最低频率 */
            power_set_pstate(pstate_count > 0 ? pstate_count - 1 : 0);
            break;
    }
}

/* === 电源事件通知 === */

struct power_event {
    int type;
    uint64_t timestamp;
    int data;
};

#define POWER_EVENT_MAX    32
static struct power_event power_events[POWER_EVENT_MAX];
static int power_event_idx = 0;

/* 记录电源事件 */
void power_record_event(int type, int data) {
    power_events[power_event_idx].type = type;
    power_events[power_event_idx].timestamp = get_tick_count();
    power_events[power_event_idx].data = data;
    power_event_idx = (power_event_idx + 1) % POWER_EVENT_MAX;
}

/* === 电池监控 === */

struct battery_info {
    int present;
    int status;         /* 0=放电, 1=充电, 2=满电 */
    int capacity;       /* 容量百分比 */
    int voltage;        /* 电压 mV */
    int current;        /* 电流 mA */
    int temperature;    /* 温度 */
    uint64_t last_update;
};

static struct battery_info battery;

/* 更新电池信息 */
void battery_update(int status, int capacity, int voltage) {
    battery.present = 1;
    battery.status = status;
    battery.capacity = capacity;
    battery.voltage = voltage;
    battery.last_update = get_tick_count();
}

/* 获取电池状态 */
int battery_get_capacity(void) {
    return battery.capacity;
}

/* 打印电池状态 */
void battery_print_status(void) {
    if (!battery.present) {
        println("Battery: Not present");
        return;
    }
    
    print("Battery: ");
    print_dec(battery.capacity);
    print("% (");
    switch (battery.status) {
        case 0: print("Discharging"); break;
        case 1: print("Charging"); break;
        case 2: print("Full"); break;
    }
    print(", ");
    print_dec(battery.voltage);
    println(" mV)");
}

/* === 电源配置文件 === */

struct power_profile {
    const char *name;
    int cpu_governor;
    int brightness;
    int timeout;        /* 休眠超时 (秒) */
};

static struct power_profile profiles[] = {
    {"Performance", DVFS_GOVERNOR_PERFORMANCE, 100, 0},
    {"Balanced",    DVFS_GOVERNOR_ONDEMAND,     70, 300},
    {"Powersave",   DVFS_GOVERNOR_POWERSAVE,    40, 60},
};

static int current_profile = 1;  /* 默认均衡模式 */

/* 设置电源配置文件 */
int power_set_profile(int profile) {
    if (profile < 0 || profile > 2) return -1;
    
    current_profile = profile;
    struct power_profile *p = &profiles[profile];
    
    /* 应用配置 */
    for (int i = 0; i < smp_cpu_count; i++) {
        dvfs_init_policy(i, p->cpu_governor);
    }
    
    power_record_event(0, profile);
    
    print("[POWER] Profile: ");
    println(p->name);
    return 0;
}

/* 获取当前配置文件 */
int power_get_profile(void) {
    return current_profile;
}

/* === 热管理 === */

struct thermal_state {
    int cpu_temp;
    int gpu_temp;
    int throttle_level;
    int fan_speed;
    int critical_temp;
};

static struct thermal_state thermal;

/* 更新温度 */
void thermal_update_temp(int cpu_temp, int gpu_temp) {
    thermal.cpu_temp = cpu_temp;
    thermal.gpu_temp = gpu_temp;
    
    /* 检查是否需要降频 */
    if (cpu_temp > thermal.critical_temp - 10) {
        thermal.throttle_level = 2;  /* 严重降频 */
    } else if (cpu_temp > thermal.critical_temp - 20) {
        thermal.throttle_level = 1;  /* 轻度降频 */
    } else {
        thermal.throttle_level = 0;
    }
}

/* 检查热节流 */
int thermal_check_throttle(void) {
    return thermal.throttle_level;
}

/* 打印热状态 */
void thermal_print_status(void) {
    println("=== Thermal Status ===");
    print("CPU Temp: "); print_dec(thermal.cpu_temp); println(" C");
    print("GPU Temp: "); print_dec(thermal.gpu_temp); println(" C");
    print("Throttle: ");
    print_dec(thermal.throttle_level);
    println("");
}

/* === 电源审计 === */

struct power_audit {
    uint64_t total_energy;      /* 总能耗 (mJ) */
    uint64_t time_on_battery;
    uint64_t time_on_ac;
    uint32_t sleep_count;
    uint32_t wake_count;
};

static struct power_audit audit;

/* 记录能耗 */
void power_audit_energy(uint64_t mw, uint64_t duration_ms) {
    audit.total_energy += mw * duration_ms;
}

/* 记录睡眠/唤醒 */
void power_audit_sleep(void) { audit.sleep_count++; }
void power_audit_wake(void) { audit.wake_count++; }

/* 打印电源审计 */
void power_print_audit(void) {
    println("=== Power Audit ===");
    print("Total Energy: "); print_dec(audit.total_energy / 1000); println(" J");
    print("Sleep Count: "); print_dec(audit.sleep_count); println("");
    print("Wake Count: "); print_dec(audit.wake_count); println("");
}

/* === 电源诊断 === */

/* 检查电源健康 */
int power_health_check(void) {
    if (!acpi_enabled) return -1;
    
    int issues = 0;
    
    /* 检查电池状态 */
    if (battery.present && battery.capacity < 10) {
        issues++;
    }
    
    /* 检查温度 */
    if (thermal.cpu_temp > thermal.critical_temp - 15) {
        issues++;
    }
    
    return issues;
}

/* 打印完整电源报告 */
void power_print_report(void) {
    println("╔══════════════════════════════════════════╗");
    println("║         Power Management Report          ║");
    println("╚══════════════════════════════════════════╝");
    
    println("");
    power_stats();
    
    println("");
    battery_print_status();
    
    println("");
    thermal_print_status();
    
    println("");
    print("Profile: ");
    println(profiles[current_profile].name);
    
    println("");
    power_print_audit();
}
