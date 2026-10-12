#ifndef UTSM_DKM_H
#define UTSM_DKM_H

#include <utsm/types.h>
#include <utsm/dma.h>
#include <utsm/block.h>
#include <utsm/net.h>
#include <utsm/paging.h>

#define DKM_DRIVER_MAGIC 0x444B4D31u
#define DKM_ABI_VERSION  1u
#define DKM_KERNEL_API_VERSION 1u

#define DKM_CLASS_PLATFORM  1u
#define DKM_CLASS_BUS       2u
#define DKM_CLASS_INTERRUPT 3u
#define DKM_CLASS_TIMER     4u
#define DKM_CLASS_CONSOLE   5u
#define DKM_CLASS_STORAGE   6u
#define DKM_CLASS_FS        7u
#define DKM_CLASS_NET       8u
#define DKM_CLASS_INPUT     9u
#define DKM_CLASS_GPU       10u
#define DKM_CLASS_MISC      11u

#define DKM_F_REQUIRED        (1u << 0)
#define DKM_F_BOOT_MODULE     (1u << 1)
#define DKM_F_NO_UNLOAD       (1u << 2)
#define DKM_F_STRONG_RECOVERY (1u << 3)
#define DKM_F_DMA_REQUIRED    (1u << 4)
#define DKM_F_EARLY_LOG       (1u << 5)

struct dkm_kernel_api;
struct dkm_driver_handle;

/* ===================================================================
 *  D3: 内核 API 按名导出表（kapi）
 *
 *  "自由也要有地图。" 固定偏移 struct ABI 之外的按名查找层：
 *  驱动/用户态可枚举内核服务并按名取函数指针。
 *  通过 kernel_api 尾部追加的 ->kapi 指针访问（ABI 兼容）。
 * =================================================================== */
typedef struct {
    const char *name;   /* 如 "log.info"、"dma.alloc_pages" */
    void *fn;           /* 服务函数指针（签名见对应服务头） */
} dkm_kapi_entry;

typedef struct {
    u32 magic;          /* DKM_KAPI_MAGIC */
    u32 version;        /* DKM_KAPI_VERSION */
    u32 count;          /* entries 数量 */
    const dkm_kapi_entry *entries;
    /* 按名查找：命中返回 0 并写 out_fn；未命中 -1 */
    int (*lookup)(const char *name, void **out_fn);
} dkm_kapi_table;

#define DKM_KAPI_MAGIC   0x4b415049u  /* "KAPI" */
#define DKM_KAPI_VERSION 1u

/* ===================================================================
 *  D6 万物互联：dkm_bus 双向服务总线
 *
 *  kapi 是内核→模块的单向只读导出表；bus 把它双向化：
 *  任何模块在 driver_init 里注册 srv.* 服务，任何模块按名消费。
 *  命名空间：kern.* 仅内核可注册，srv.* 仅模块可注册（BUS-E01）。
 *  失败策略：lookup miss = BUS-E04 panic（硬依赖未满足=编程错误）；
 *  probe   miss = 返回 -1（能力缺席=设计内双路径，白名单登记）。
 *  生命周期：driver_init = 注册服务；"运行"也是服务（srv.<mod>.run）。
 *  设计定稿：docs/RE/万物互联与服务总线设计.md §1。
 * =================================================================== */
#define DKM_BUS_MAGIC    0x53425553u  /* "SBUS" */
#define DKM_BUS_VERSION  1u
#define DKM_BUS_MAX_SERVICES 128
#define DKM_BUS_MAX_NAME 48

#define DKM_BUS_F_KERNEL    1u  /* 内核注册，不可注销 */
#define DKM_BUS_F_NO_UNLOAD 2u  /* provider 声明 NO_UNLOAD，注销即 BUS-E05 */

typedef struct dkm_bus_service {
    char name[DKM_BUS_MAX_NAME];   /* NUL 终止；必须带命名空间前缀 */
    u32 version;                   /* 服务契约版本，消费方校验 */
    u32 flags;                     /* DKM_BUS_F_* */
    void *fn;                      /* 服务入口，签名由各服务契约定义 */
    const struct dkm_driver_desc *provider;  /* NULL = 内核；存 desc 而非
                                              * handle——loader 的 handle 是栈
                                              * 局部变量会悬垂，desc 在驱动
                                              * 镜像内存里持久且每驱动唯一 */
} dkm_bus_service;

typedef struct dkm_bus_api {
    u32 magic;
    u32 version;
    /* 注册：name 必须带命名空间前缀且与调用方身份匹配（BUS-E01）、
     * 重名 BUS-E02、表满 BUS-E03——全部 panic，成功返回 0。 */
    int (*register_service)(const char *name, u32 version, void *fn);
    /* 注销：仅 provider 本人可注销；F_KERNEL/F_NO_UNLOAD 或不存在 → BUS-E05。 */
    int (*unregister_service)(const char *name);
    /* 必存在查找：未命中 → BUS-E04 panic，绝不返回 NULL 伪装成功。 */
    int (*lookup)(const char *name, void **out_fn, u32 *out_version);
    /* 可选探测：未命中返回 -1；调用方必须显式处理缺席（白名单登记）。 */
    int (*probe)(const char *name, void **out_fn, u32 *out_version);
    u32 (*count)(void);                           /* 诊断 / /dev/services */
    const dkm_bus_service *(*at)(u32 index);      /* 按 index 枚举 */
} dkm_bus_api;

typedef struct dkm_driver_desc {
    u32 magic;
    u16 abi_version;
    u16 desc_size;

    const char *name;
    const char *version;
    const char *vendor;

    u32 driver_class;
    u32 stage;
    u32 flags;
    u32 priority;

    const char *const *depends;
    u32 depends_count;

    const char *const *provides;
    u32 provides_count;

    u64 min_kernel_abi;
    u64 feature_bits;

    u64 reserved0;
    u64 reserved1;
} dkm_driver_desc;

typedef int (*dkm_driver_init_fn)(const struct dkm_kernel_api *api, struct dkm_driver_handle *handle);
typedef int (*dkm_driver_exit_fn)(struct dkm_driver_handle *handle);

/* DRR 恢复根服务表（Phase8；驱动经 kernel_api->drr 使用） */
typedef struct drr_recovery_api {
    void (*report_fault)(const char *reason);
    u64  (*heartbeat_get)(void);
    void *(*emergency_alloc)(u64 size, u64 alignment);
    int  (*ckpt_snapshot_dirty)(void);
    int  (*rollback_pages)(void);
} drr_recovery_api;

/* SAS-R0-PCQ 调度器服务表（Phase7；驱动经 kernel_api->sched 使用） */
typedef struct utsm_sched_api {
    u64  (*uptime_ns)(void);
    void (*yield)(void);
    u32  (*current_task_slot)(void);
    int  (*task_create)(u32 process_slot, u32 priority, void (*entry)(void *), void *arg);
    void (*sleep_ms)(u32 ms);
} utsm_sched_api;

typedef struct dkm_log_api {
    void (*info)(const char *msg);
    void (*warn)(const char *msg);
    void (*error)(const char *msg);
    void (*panic)(const char *msg);
} dkm_log_api;

typedef struct dkm_kernel_api {
    u32 version;
    u32 size;
    u64 feature_bits;

    const dkm_log_api *log;
    const void *mem;
    const void *utsm;
    const void *irq;
    const void *pci;
    const dkm_dma_api *dma;
    const void *vfs;
    const dkm_net_api *net;
    const void *timer;
    const drr_recovery_api *drr;   /* 恢复根服务（原占位 void* 改具体类型，布局不变） */

    const void *rsdp_address;   /* ACPI RSDP physical address */
    const void *fb_address;     /* framebuffer base */
    u64 fb_width;
    u64 fb_height;
    u64 fb_pitch;
    u16 fb_bpp;
    const void *boot_modules_response;  /* limine_module_response* */
    int (*irq_register)(u8 irq, void *handler);  /* kernel irq_register */
    u64 hhdm_offset;                               /* physical → virtual */
    const dkm_block_api *block;                    /* block provider registry */
    const dkm_mmio_api *mmio;                      /* 页表/高位 MMIO 映射服务（尾部追加，ABI 兼容） */
    /* —— 新增字段只允许追加在末尾（DSK 以 api+0xA8 硬偏移读取 block） —— */
    void *(*mm_map_mmio)(u64 phys, u64 size);      /* MMIO 独立窗口映射（PCD|PWT），返回虚拟地址，失败返回 0 */
    void (*mm_unmap_mmio)(void *virt, u64 size);   /* 解除 MMIO 独立窗口映射 */
    /* B7 阶段2: 驱动向 UTSM 注册 LAPIC EOI 钩子（写 LAPIC EOI 寄存器的函数指针）。
     * apic.drv 在 LAPIC 接管时注册；idt_handler 的 IRQ 分发末尾调用该钩子，
     * 使 vector 0x20-0x2F 的 legacy 线在 IOAPIC 路由下可持续投递。
     * 未注册（PIC 路由）时钩子为 NULL，idt_handler 跳过，行为与旧版一致。 */
    void (*register_apic_eoi)(void (*eoi_fn)(void));
    /* B7 阶段3: 动态 IDT 向量分配器（MSI/MSI-X 用），尾部追加保持 ABI。
     * 池范围 0x40-0xDF；alloc 返回向量号、失败 -1；free 幂等越界忽略。
     * 驱动拿到向量后经 irq_register(vector, handler) 直接注册（现有
     * 接口已接受向量命名空间，无需新注册 ABI）。 */
    int  (*irq_vector_alloc)(void);
    void (*irq_vector_free)(int vector);
    /* M4/Phase7-8: 调度器服务（尾部追加，ABI 兼容；drr 服务见上方原占位字段）。
     * 驱动可用：task_create/uptime/sleep/current_task_slot。
     * DSK 以 api+0xA8 硬偏移读 block 的约定不受影响（仅尾部追加）。 */
    const utsm_sched_api *sched;
    /* D3: 按名导出表（尾部追加，ABI 兼容）。驱动/用户态枚举内核服务并
     * 按名取函数指针；固定偏移 struct 字段不受影响。 */
    const dkm_kapi_table *kapi;
    /* D6 万物互联：双向服务总线（尾部追加，ABI 兼容）。kern.* 与 srv.* 按名
     * 注册-消费；driver_init = 注册服务的地方。 */
    const dkm_bus_api *bus;
} dkm_kernel_api;

typedef enum dkm_driver_state {
    DKM_STATE_DISCOVERED = 0,
    DKM_STATE_ABI_CHECKED,
    DKM_STATE_INITING,
    DKM_STATE_ACTIVE,
    DKM_STATE_FAILED
} dkm_driver_state;

typedef struct dkm_driver_handle {
    const dkm_driver_desc *desc;
    dkm_driver_state state;
    i32 init_status;
    u32 load_stage;
    u32 flags;
} dkm_driver_handle;

typedef struct dkm_builtin_driver {
    const dkm_driver_desc *desc;
    dkm_driver_init_fn init;
    dkm_driver_exit_fn exit;
} dkm_builtin_driver;

void dkm_init(void);
/* D4: 设置当前驱动日志颜色上下文（driver_init 前设置，返回后清 0） */
void dkm_log_set_driver(const char *name);

int dkm_load_builtin(const dkm_builtin_driver *driver);
void dkm_scan_boot_modules(void);
void dsm_load_by_manifest(void);
/* P0（万物互联）：按名加载。dsm_load_named 严格版（depends 校验强制，供
 * kern.dkm.load_by_name 等常规路径）；dsm_load_named_boot 顺序豁免版
 * （唯一合法调用方 = kernel/main.c VMM 路径 pci+ahci 预加载，先于 apic，
 * 否则 guest 陷入 IDTR=0 VM-exit 风暴——设计内豁免，见
 * docs/RE/万物互联与服务总线设计.md §6）。
 * 签名（ini_config 定义在 kernel/ini_parser.h，此处避免头依赖）：
 *   void dsm_load_named(const ini_config *cfg, const char *name);
 *   void dsm_load_named_boot(const ini_config *cfg, const char *name);
 * 调用方自行前向声明（同 kernel/main.c 既有模式）。 */
void dkm_fill_platform_info(void);
const dkm_kernel_api *dkm_get_kernel_api(void);

/* 服务表实现（drr_stub.c / sched/sched.c 提供，fill 时挂入 kernel_api） */
const struct drr_recovery_api *drr_get_recovery_api(void);
const struct utsm_sched_api *utsm_sched_get_api(void);

/* D6 万物互联：服务总线（dkm/bus.c 提供）。dkm_bus_init 播种 kern.*
 * 并挂入 kernel_api（dkm_fill_platform_info 内调用，先于一切驱动加载）；
 * loader 在 driver_init 前后 set/clear provider 上下文（仿 D4 颜色上下文），
 * kern.* 只能在 provider==NULL 时注册。 */
void dkm_bus_init(void);
const dkm_bus_api *dkm_bus_get_api(void);
void dkm_bus_set_current_provider(struct dkm_driver_handle *h);
/* loader 专用：token T 是否已有 "prefix" 或 "prefix.*" 服务注册
 * （stage>=4 provides 校验用，DKM-E45）。命中返回 1。 */
int dkm_bus_prefix_registered(const char *prefix);

/* exposed for manifest parser */
struct dkm_symbol_scan {
    const void *driver_desc;
    const void *driver_init;
    const void *driver_exit;
};
int dkm_check_elf64(const void *address, u64 size);
int dkm_scan_symbols(const void *address, u64 size, struct dkm_symbol_scan *out);
int dkm_load_elf_rel(const void *address, u64 size, const struct dkm_symbol_scan *symbols);

extern const dkm_builtin_driver dkm_builtin_console_early;

#endif
