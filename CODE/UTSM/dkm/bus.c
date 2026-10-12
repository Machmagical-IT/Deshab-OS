/* ===================================================================
 *  D6 万物互联：dkm_bus 双向服务总线
 *
 *  "没有主内核，每个模块都是内核——总线就是万物的神经。"
 *
 *  kapi（kernel_api.c）是内核→模块的单向只读导出表；本文件把它双向化：
 *  任何 .drv 模块在 driver_init 里注册 srv.* 服务，任何模块按名消费。
 *  kern.* 仅内核可注册（provider==NULL 时），srv.* 仅模块可注册。
 *
 *  失败策略（铁律映射）：
 *    register 名字非法/身份不符 → BUS-E01 panic（编程错误）
 *    register 重名             → BUS-E02 panic
 *    register 表满             → BUS-E03 panic
 *    lookup  未命中            → BUS-E04 panic（硬依赖未满足=编程错误）
 *    unregister 违规           → BUS-E05 panic
 *    probe   未命中            → 返回 -1（能力缺席=设计内双路径，白名单）
 *
 *  设计定稿：docs/RE/万物互联与服务总线设计.md §1。
 * =================================================================== */

#include <utsm/dkm.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include <utsm/idt.h>
#include <utsm/mm.h>
#include <utsm/dma.h>
#include <utsm/drr.h>
#include <utsm/utrw.h>
#include <utsm/block.h>
#include <utsm/pe.h>
#include <utsm/linux_compat.h>
#include "../kernel/ini_parser.h"

/* manifest.c 提供；签名见 dkm.h 头注释（避免头依赖） */
void dsm_load_named(const ini_config *cfg, const char *name);
/* kernel/main.c 提供：FUCK 启动配置只读访问（g_boot_cfg） */
const ini_config *utsm_boot_cfg_get(void);

static dkm_bus_service g_services[DKM_BUS_MAX_SERVICES];
static u32 g_service_count;
static struct dkm_driver_handle *g_current_provider; /* NULL = 内核 */

/* ---- 名字工具 ---- */

static int bus_streq(const char *a, const char *b) {
    usize i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static int bus_prefix_of(const char *s, const char *prefix) {
    usize i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static int bus_find(const char *name) {
    for (u32 i = 0; i < g_service_count; i++) {
        if (bus_streq(g_services[i].name, name)) return (int)i;
    }
    return -1;
}

/* ---- 总线原语（BUS-E 全部 panic，零降级） ---- */

static int bus_register_impl(const char *name, u32 version, void *fn) {
    if (!name || !fn || !name[0]) {
        panic_full("BUS-E01 INVALID SERVICE NAME",
                   "dkm_bus: null/empty service name or fn", 0);
        return -1;
    }
    usize len = 0;
    while (name[len]) {
        if (len >= DKM_BUS_MAX_NAME) {
            log_error("[BUS] name too long: ");
            log_info(name);
            panic_full("BUS-E01 INVALID SERVICE NAME",
                       "dkm_bus: service name exceeds DKM_BUS_MAX_NAME", 0);
            return -1;
        }
        len++;
    }
    int is_kern = bus_prefix_of(name, "kern.");
    int is_srv  = bus_prefix_of(name, "srv.");
    if (!is_kern && !is_srv) {
        log_error("[BUS] name without namespace prefix: ");
        log_info(name);
        panic_full("BUS-E01 INVALID SERVICE NAME",
                   "dkm_bus: service name must start with kern. or srv.", 0);
        return -1;
    }
    if (is_kern && g_current_provider != 0) {
        log_error("[BUS] module tried to register kern.*: ");
        log_info(name);
        panic_full("BUS-E01 INVALID SERVICE NAME",
                   "dkm_bus: only the kernel may register kern.* services", 0);
        return -1;
    }
    if (is_srv && g_current_provider == 0) {
        log_error("[BUS] kernel tried to register srv.*: ");
        log_info(name);
        panic_full("BUS-E01 INVALID SERVICE NAME",
                   "dkm_bus: kernel services must use the kern.* namespace", 0);
        return -1;
    }
    if (bus_find(name) >= 0) {
        log_error("[BUS] duplicate service: ");
        log_info(name);
        panic_full("BUS-E02 DUPLICATE SERVICE",
                   "dkm_bus: service name already registered", 0);
        return -1;
    }
    if (g_service_count >= DKM_BUS_MAX_SERVICES) {
        panic_full("BUS-E03 SERVICE TABLE FULL",
                   "dkm_bus: g_services overflow (DKM_BUS_MAX_SERVICES)", 0);
        return -1;
    }

    dkm_bus_service *s = &g_services[g_service_count];
    for (usize i = 0; i < len; i++) s->name[i] = name[i];
    s->name[len] = 0;
    s->version = version;
    s->fn = fn;
    /* 存 desc 而非 handle：loader 的 handle 是栈局部（init 返回即悬垂），
     * desc 在驱动镜像内存里持久；注销校验按 desc 指针判身份。 */
    s->provider = g_current_provider ? g_current_provider->desc : 0;
    if (is_kern) {
        s->flags = DKM_BUS_F_KERNEL;
    } else if (g_current_provider && (g_current_provider->flags & DKM_F_NO_UNLOAD)) {
        s->flags = DKM_BUS_F_NO_UNLOAD;
    } else {
        s->flags = 0;
    }
    g_service_count++;

    log_info("[BUS] service registered: ");
    log_info(name);
    return 0;
}

static int bus_unregister_impl(const char *name) {
    int idx = name ? bus_find(name) : -1;
    if (idx < 0) {
        log_error("[BUS] unregister unknown service: ");
        log_info(name ? name : "(null)");
        panic_full("BUS-E05 UNREGISTER VIOLATION",
                   "dkm_bus: unregister of unknown service", 0);
        return -1;
    }
    dkm_bus_service *s = &g_services[idx];
    if (s->flags & (DKM_BUS_F_KERNEL | DKM_BUS_F_NO_UNLOAD)) {
        log_error("[BUS] unregister of permanent service: ");
        log_info(name);
        panic_full("BUS-E05 UNREGISTER VIOLATION",
                   "dkm_bus: F_KERNEL/F_NO_UNLOAD service cannot be unregistered", 0);
        return -1;
    }
    if (s->provider != (g_current_provider ? g_current_provider->desc : 0)) {
        log_error("[BUS] unregister by non-provider: ");
        log_info(name);
        panic_full("BUS-E05 UNREGISTER VIOLATION",
                   "dkm_bus: only the provider may unregister its service", 0);
        return -1;
    }
    /* 紧缩（顺序无关紧要，表小） */
    for (u32 i = (u32)idx; i + 1 < g_service_count; i++) {
        g_services[i] = g_services[i + 1];
    }
    g_service_count--;
    log_info("[BUS] service unregistered: ");
    log_info(name);
    return 0;
}

static int bus_lookup_impl(const char *name, void **out_fn, u32 *out_version) {
    int idx = name ? bus_find(name) : -1;
    if (idx < 0) {
        log_error("[BUS] lookup miss: ");
        log_info(name ? name : "(null)");
        panic_full("BUS-E04 LOOKUP MISS",
                   "dkm_bus: required service not registered (hard dependency)", 0);
        return -1;
    }
    if (out_fn) *out_fn = g_services[idx].fn;
    if (out_version) *out_version = g_services[idx].version;
    return 0;
}

static int bus_probe_impl(const char *name, void **out_fn, u32 *out_version) {
    int idx = name ? bus_find(name) : -1;
    if (idx < 0) return -1;
    if (out_fn) *out_fn = g_services[idx].fn;
    if (out_version) *out_version = g_services[idx].version;
    return 0;
}

static u32 bus_count_impl(void) {
    return g_service_count;
}

static const dkm_bus_service *bus_at_impl(u32 index) {
    if (index >= g_service_count) return 0;
    return &g_services[index];
}

static const dkm_bus_api g_bus_api = {
    .magic = DKM_BUS_MAGIC,
    .version = DKM_BUS_VERSION,
    .register_service = bus_register_impl,
    .unregister_service = bus_unregister_impl,
    .lookup = bus_lookup_impl,
    .probe = bus_probe_impl,
    .count = bus_count_impl,
    .at = bus_at_impl,
};

const dkm_bus_api *dkm_bus_get_api(void) {
    return &g_bus_api;
}

void dkm_bus_set_current_provider(struct dkm_driver_handle *h) {
    g_current_provider = h;
}

int dkm_bus_prefix_registered(const char *prefix) {
    if (!prefix) return 0;
    usize len = 0;
    while (prefix[len]) len++;
    for (u32 i = 0; i < g_service_count; i++) {
        const char *n = g_services[i].name;
        if (bus_streq(n, prefix)) return 1;
        /* 前缀命中：n == "prefix.xxx" */
        if (bus_prefix_of(n, prefix) && n[len] == '.') return 1;
    }
    return 0;
}

/* ===================================================================
 *  kern.* 播种
 *
 *  kapi 14 条（log.* dma/irq/mm/apic/drr/utrw）以 kern. 前缀重新播种
 *  （旧无前缀 kapi 名冻结不动，兼容层）；另加 block/pe/lxc/probe/config/
 *  load_by_name。dsk_boot_context.reserved[] 的逐槽替代服务（srv.*）随
 *  P2-P4 各模块落地注册，本表不抢跑。
 *  kern.probe.info 随 P4b（shell 消费方）落地；kern.bootlog.milestone
 *  随 P2（fat32.drv 实装）落地。
 * =================================================================== */

static void bus_kern_panic(const char *msg) {
    panic_full("DKM-PANIC", msg, 0);
}

static int bus_kern_load_by_name(const char *name) {
    /* P3 接线后带 FUCK cfg 过滤；当前调用方（内部）走 manifest 语义。 */
    dsm_load_named(0, name);
    return 0;
}

static const char *bus_kern_config_get(const char *section, const char *key) {
    const ini_config *cfg = utsm_boot_cfg_get();
    if (!cfg) return 0;
    return ini_get(cfg, section, key);
}

static int bus_seed(const char *name, void *fn) {
    return bus_register_impl(name, 1u, fn);
}

void dkm_bus_init(void) {
    static int s_inited = 0;
    if (s_inited) return;
    s_inited = 1;

    g_service_count = 0;
    g_current_provider = 0;

    /* log（kapi 4 条迁移） */
    bus_seed("kern.log.info",  (void *)log_info);
    bus_seed("kern.log.warn",  (void *)log_warn);
    bus_seed("kern.log.error", (void *)log_error);
    bus_seed("kern.log.panic", (void *)bus_kern_panic);
    /* dma / irq / mm / apic（kapi 迁移 + P6 真 IRQ 输入新增 unmask/mask） */
    bus_seed("kern.dma.alloc_pages", (void *)dma_alloc_pages);
    bus_seed("kern.irq.register",    (void *)irq_register);
    bus_seed("kern.irq.vector_alloc", (void *)irq_vector_alloc);
    bus_seed("kern.irq.vector_free",  (void *)irq_vector_free);
    bus_seed("kern.irq.unmask", (void *)pic_irq_unmask);
    bus_seed("kern.irq.mask",   (void *)pic_irq_mask);
    bus_seed("kern.mm.map_mmio",   (void *)mm_map_mmio);
    bus_seed("kern.mm.unmap_mmio", (void *)mm_unmap_mmio);
    bus_seed("kern.apic.register_eoi", (void *)idt_register_apic_eoi);
    /* drr / utrw（kapi 迁移） */
    bus_seed("kern.drr.snapshot_dirty", (void *)drr_ckpt_snapshot_dirty);
    bus_seed("kern.drr.rollback_pages", (void *)drr_rollback_pages);
    bus_seed("kern.utrw.debug_dump",    (void *)utrw_debug_dump_page);
    /* block registry 前端（替代 desktop 的 api+0xA8 裸偏移读） */
    {
        const dkm_block_api *b = block_get_api();
        bus_seed("kern.block.read",  (void *)b->read);
        bus_seed("kern.block.write", (void *)b->write);
    }
    /* PE / Linux 兼容层服务（替代 reserved[4]/reserved[5]） */
    bus_seed("kern.pe.service",  (void *)pe_get_service);
    bus_seed("kern.lxc.service", (void *)linux_compat_get_service);
    /* 按名加载（session.drv 的排序拉手，P3 接 FUCK 过滤） */
    bus_seed("kern.dkm.load_by_name", (void *)bus_kern_load_by_name);
    /* FUCK 配置只读访问（替代 reserved[] 内散落的配置回传） */
    bus_seed("kern.config.get", (void *)bus_kern_config_get);

    log_info("[BUS] ready services=");
    log_hex64("[BUS] count=", g_service_count);
}
