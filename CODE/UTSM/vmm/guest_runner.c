/* guest_runner.c — AP 后台 guest 运行器。
 *
 * BSP 先拉起桌面，再由 session.drv 调 kern.guest.start：
 *   - FUCK [boot] smp=0 或 guest_background=0 → 返回 0，main.c 走旧同步路径
 *   - 否则 smp_boot_ap(1) 启 AP，AP 上顺序：
 *       Linux: ipc_shm_init → linux_loader_init → linux_launch → lib/vscode
 *       Windows: prepare → (autostart) launch
 *     之后循环服务 BSP 的 Linux 兼容层邮箱请求。
 */
#include <utsm/guest_runner.h>
#include <utsm/guest_progress.h>
#include <utsm/smp.h>
#include <utsm/vmx.h>
#include <utsm/vmm.h>
#include <utsm/vm_machine.h>
#include <utsm/linux_loader.h>
#include <utsm/linux_compat.h>
#include <utsm/windows_guest.h>
#include <utsm/ipc_shm.h>
#include <utsm/panic.h>
#include <utsm/log.h>
#include <utsm/sched_ext.h>
#include "../kernel/ini_parser.h"

extern const ini_config *utsm_boot_cfg_get(void);
void dkm_bus_freeze(void);

extern char g_compat_linux_guest_path[64];
extern int g_compat_linux_lib_sync;
extern int g_compat_vscode_install;

#define AP_CPU_INDEX 1u

static volatile int g_ap_started;
static volatile u32 g_ap_cpu;

/* 单槽邮箱：BSP 单生产者（桌面调用串行），AP 单消费者。 */
static struct {
    volatile u32 req;
    volatile u32 op;
    u64 a[8];
    volatile i64 rc;
} g_mail;

int guest_runner_active(void) { return g_ap_started; }

int guest_runner_on_owner_cpu(void) {
    if (!g_ap_started) return 1;   /* legacy：BSP 直接执行 */
    return smp_current_cpu() == g_ap_cpu;
}

static void ap_service_once(void) {
    if (__atomic_load_n(&g_mail.req, __ATOMIC_ACQUIRE) == 0) return;
    u32 op = __atomic_load_n(&g_mail.op, __ATOMIC_RELAXED);
    u64 a[8];
    for (u32 i = 0; i < 8; i++) a[i] = g_mail.a[i];
    i64 rc = linux_compat_ap_dispatch(op, a, 8);
    __atomic_store_n(&g_mail.rc, rc, __ATOMIC_RELAXED);
    __atomic_store_n(&g_mail.req, 0, __ATOMIC_RELEASE);
}

static void ap_service_loop(void) {
    for (;;) {
        ap_service_once();
        __asm__ volatile("pause");
    }
}

i64 guest_runner_call_ex(u32 op, const u64 *args, u32 nargs, int wait) {
    if (!g_ap_started) return -1;
    if (smp_current_cpu() == g_ap_cpu) {
        /* 已在 AP 上（Linux 兼容层内部调用）：直接执行，避免自投递死锁。 */
        return linux_compat_ap_dispatch(op, args, nargs);
    }
    if (__atomic_load_n(&g_mail.req, __ATOMIC_ACQUIRE) != 0) {
        if (!wait) return -2;
        while (__atomic_load_n(&g_mail.req, __ATOMIC_ACQUIRE) != 0) {
            utsm_sched_sleep_ms(1);
        }
    }
    for (u32 i = 0; i < 8; i++) g_mail.a[i] = (i < nargs) ? args[i] : 0;
    __atomic_store_n(&g_mail.op, op, __ATOMIC_RELAXED);
    __atomic_store_n(&g_mail.rc, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_mail.req, 1, __ATOMIC_RELEASE);
    if (!wait) return 0;
    while (__atomic_load_n(&g_mail.req, __ATOMIC_ACQUIRE) != 0) {
        utsm_sched_sleep_ms(1);
    }
    return __atomic_load_n(&g_mail.rc, __ATOMIC_ACQUIRE);
}

i64 guest_runner_call(u32 op, const u64 *args, u32 nargs) {
    return guest_runner_call_ex(op, args, nargs, 1);
}

/* ---------------- AP 侧运行器 ---------------- */

static void ap_guest_main(void *arg) {
    (void)arg;
    g_ap_cpu = smp_current_cpu();
    log_hex64("[GUEST] AP cpu=", g_ap_cpu);

    if (vmx_cpu_enable(g_ap_cpu) != 0) {
        panic_full("SMP-E03 AP VMX ENABLE FAILED",
                   "the AP could not enter VMX root mode", 0);
        ap_service_loop();
    }

    /* 抢占 VMCS 所有权：BSP 自检后已 release，AP 在这里成为唯一 owner。 */
    {
        struct vm_machine *m = vm_machine_active();
        if (!m || vm_machine_activate(m, 0) != 0) {
            panic_full("GUEST-E04 AP VMCS CLAIM FAILED",
                       "the AP could not claim the legacy VMCS from the BSP", 0);
            ap_service_loop();
        }
    }

    const ini_config *cfg = utsm_boot_cfg_get();
    int linux = ini_get_bool(cfg, "boot", "linux_guest", 1);
    int windows = ini_get_bool(cfg, "boot", "windows_guest", 0);
    int win_auto = ini_get_bool(cfg, "boot", "windows_autostart", 0);

    if (linux) {
        guest_progress_set(GUEST_SLOT_LINUX, 1, GUEST_PHASE_PREP, 5, 0);
        if (ipc_shm_init() != 0) {
            panic_full("LNX-E03 IPC SHM INIT FAILED",
                       "AP guest runner: IPC shared memory init failed", 0);
            guest_progress_set(GUEST_SLOT_LINUX, 1, GUEST_PHASE_FAILED, 5, 0);
        } else if (linux_loader_init() != 0) {
            panic_full("VMM-E01 LINUX BZIMAGE MISSING",
                       "linux_guest=1 but bzImage boot module not found", 0);
            guest_progress_set(GUEST_SLOT_LINUX, 1, GUEST_PHASE_FAILED, 5, 0);
        } else {
            log_info("[GUEST] linux loader init ok");
            guest_progress_set(GUEST_SLOT_LINUX, 1, GUEST_PHASE_LAUNCH, 35, 0);
            int st = linux_launch();
            if (st != 0) {
                log_hex64("[GUEST] linux launch st=", (u64)(i64)st);
                guest_progress_set(GUEST_SLOT_LINUX, 1, GUEST_PHASE_FAILED, 35, 0);
                panic_full("VMM-E02 LINUX LAUNCH FAILED",
                           "linux_guest=1 requested but VMX launch failed", 0);
            } else {
                log_info("[GUEST] linux parked (daemon ready)");
                linux_compat_init();
                guest_progress_set(GUEST_SLOT_LINUX, 1, GUEST_PHASE_POST, 92, 0);
                if (g_compat_linux_lib_sync) {
                    if (lxc_sync_lib_dir(g_compat_linux_guest_path) != 0) {
                        panic_full("LNX-E01 LIB SYNC FAILED",
                                   "FAT32 lib dir sync to Linux guest failed", 0);
                    }
                }
                if (g_compat_vscode_install) {
                    if (lxc_vscode_install() != 0) {
                        panic_full("LNX-E02 VSCODE INSTALL FAILED",
                                   "vscode tarball install into guest failed", 0);
                    }
                }
                guest_progress_set(GUEST_SLOT_LINUX, 1, GUEST_PHASE_READY, 100, 0);
                log_info("[GUEST] linux ready");
            }
        }
    } else {
        guest_progress_set(GUEST_SLOT_LINUX, 0, GUEST_PHASE_OFF, 0, 0);
    }

    /* Linux 就绪后先服务一轮桌面请求，再进入 Windows（若请求）。 */
    ap_service_once();

    if (windows) {
        guest_progress_set(GUEST_SLOT_WINDOWS, 1, GUEST_PHASE_PREP, 5, 0);
        if (windows_guest_prepare() != 0) {
            guest_progress_set(GUEST_SLOT_WINDOWS, 1, GUEST_PHASE_FAILED, 5, 0);
            panic_full("WIN-E23 WINDOWS PREPARE FAILED",
                       "windows_guest=1 but OVMF platform preparation failed", 0);
        } else {
            guest_progress_set(GUEST_SLOT_WINDOWS, 1, GUEST_PHASE_PREP, 50, 1);
            log_info("[GUEST] windows prepared");
            if (win_auto) {
                guest_progress_set(GUEST_SLOT_WINDOWS, 1, GUEST_PHASE_LAUNCH, 55, 0);
                int wst = windows_guest_launch();
                if (wst != 0) {
                    guest_progress_set(GUEST_SLOT_WINDOWS, 1, GUEST_PHASE_FAILED, 55, 0);
                    panic_full("WIN-E24 WINDOWS AUTOSTART FAILED",
                               "Windows OVMF launch failed", 0);
                } else {
                    guest_progress_set(GUEST_SLOT_WINDOWS, 1, GUEST_PHASE_READY, 100, 0);
                }
            } else {
                log_info("[GUEST] windows prepared (autostart=0)");
            }
        }
    } else {
        guest_progress_set(GUEST_SLOT_WINDOWS, 0, GUEST_PHASE_OFF, 0, 0);
    }

    ap_service_loop();
}

int guest_runner_start(void) {
    const ini_config *cfg = utsm_boot_cfg_get();
    int vmm = ini_get_bool(cfg, "boot", "vmm", 1);
    int smp = ini_get_bool(cfg, "boot", "smp", 1);
    int background = ini_get_bool(cfg, "boot", "guest_background", 1);
    int linux = ini_get_bool(cfg, "boot", "linux_guest", 1);
    int windows = ini_get_bool(cfg, "boot", "windows_guest", 0);
    int xj380 = ini_get_bool(cfg, "boot", "xj380_guest", 0);
    int zt5 = ini_get_bool(cfg, "zt5", "enable", 0);

    /* R5：调度器已接管全部 AP 时不能再 INIT-SIPI 抢占某个 AP；
     * main.c 已把 guest_background_mode 降级为 0（同步路径已跑过 guest），
     * 这里直接返回"未处理"，避免二次启动。 */
    if (smp_is_enabled()) {
        log_info("[GUEST] scheduler SMP owns APs; background mode skipped");
        return 0;
    }
    if (!vmm || !smp || !background || (!linux && !windows)) return 0;
    if (!vmm_is_ready()) {
        log_warn("[GUEST] VMM not ready; background guest skipped");
        return 0;
    }

    if (xj380 || zt5) {
        panic_full("GUEST-E02 GUEST COMBINATION UNSUPPORTED",
                   "background mode supports only Linux + Windows guests", 0);
        return 0;
    }
    if (smp_init() != 0) {
        panic_full("SMP-E02 AP START TIMEOUT",
                   "smp=1 but MADT exposes no second enabled CPU", 0);
        return 0;
    }
    if (smp_prepare_guest_ap(AP_CPU_INDEX) != 0) {
        panic_full("SMP-E05 AP SLOT PREPARE FAILED",
                   "could not prepare the per-cpu slot for the guest runner", 0);
        return 0;
    }

    /* BSP 自检已完成；把 VMCS 所有权交给 AP。 */
    vm_machine_release_cpu();
    dkm_bus_freeze();
    g_ap_started = 1;
    g_ap_cpu = AP_CPU_INDEX;
    log_info("[GUEST] starting AP guest runner");
    if (smp_boot_ap(AP_CPU_INDEX, ap_guest_main, 0) != 0) {
        g_ap_started = 0;
        panic_full("SMP-E02 AP START TIMEOUT",
                   "smp_boot_ap failed to bring the guest runner online", 0);
        return 0;
    }
    return 1;
}
