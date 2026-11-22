/* guest_progress.c — AP guest 运行器 → BSP 桌面的进度共享表。
 *
 * 写者只有 AP guest_runner；BSP 只读。所有字段 32 位，使用 __atomic
 * 读写，避免结构体拷贝撕裂。 */
#include <utsm/guest_progress.h>
#include <utsm/vm_machine.h>

static utsm_guest_progress_table g_table = {
    .magic = GUEST_PROGRESS_MAGIC,
    .abi_version = 1,
    .count = GUEST_SLOT_COUNT,
};

static char g_hist[64];
static u32 g_hist_n;

const utsm_guest_progress_table *guest_progress_get(void) {
    return &g_table;
}

void guest_progress_set(u32 slot, u32 enabled, u32 phase, u32 percent, u32 detail) {
    if (slot >= GUEST_SLOT_COUNT) return;
    utsm_guest_progress *p = &g_table.slot[slot];
    u32 old = __atomic_load_n(&p->percent, __ATOMIC_ACQUIRE);
    int force = (phase == GUEST_PHASE_READY || phase == GUEST_PHASE_FAILED);
    if (!force && old != GUEST_PROGRESS_PCT_UNKNOWN && percent < old) {
        percent = old;
    }
    __atomic_store_n(&p->magic, GUEST_PROGRESS_MAGIC, __ATOMIC_RELAXED);
    __atomic_store_n(&p->enabled, enabled, __ATOMIC_RELEASE);
    __atomic_store_n(&p->detail, detail, __ATOMIC_RELAXED);
    __atomic_store_n(&p->phase, phase, __ATOMIC_RELAXED);
    __atomic_store_n(&p->percent, percent, __ATOMIC_RELEASE);
}

static void hist_push(char c) {
    if (g_hist_n < sizeof(g_hist)) {
        g_hist[g_hist_n++] = c;
    } else {
        for (u32 i = 0; i + 1 < sizeof(g_hist); i++) g_hist[i] = g_hist[i + 1];
        g_hist[sizeof(g_hist) - 1] = c;
    }
}

static int hist_match(const char *s, u32 len) {
    if (len == 0 || len > sizeof(g_hist) || g_hist_n < len) return 0;
    for (u32 i = 0; i < len; i++) {
        if (g_hist[g_hist_n - len + i] != s[i]) return 0;
    }
    return 1;
}

/* Linux/Windows guest 的串口关键行 → 进度锚点。
 * 每行只在对应 profile 激活时生效，避免两个 guest 串台。 */
void guest_progress_note_serial(char c) {
    hist_push(c);

    if (vm_machine_profile_active(VM_PROFILE_LINUX)) {
        if (hist_match("Linux version", 13))
            guest_progress_set(GUEST_SLOT_LINUX, 1, GUEST_PHASE_KERNEL, 55, 1);
        else if (hist_match("Freeing unused kernel memory", 28))
            guest_progress_set(GUEST_SLOT_LINUX, 1, GUEST_PHASE_KERNEL, 75, 2);
        else if (hist_match("Run /sbin/init", 14))
            guest_progress_set(GUEST_SLOT_LINUX, 1, GUEST_PHASE_DAEMON, 85, 3);
        else if (hist_match("systemd", 7))
            guest_progress_set(GUEST_SLOT_LINUX, 1, GUEST_PHASE_DAEMON, 88, 3);
    } else if (vm_machine_profile_active(VM_PROFILE_WINDOWS)) {
        if (hist_match("Microsoft", 9))
            guest_progress_set(GUEST_SLOT_WINDOWS, 1, GUEST_PHASE_KERNEL, 65, 1);
        else if (hist_match("Boot Manager", 12))
            guest_progress_set(GUEST_SLOT_WINDOWS, 1, GUEST_PHASE_KERNEL, 75, 2);
        else if (hist_match("Windows", 7))
            guest_progress_set(GUEST_SLOT_WINDOWS, 1, GUEST_PHASE_KERNEL, 85, 3);
    }
}
