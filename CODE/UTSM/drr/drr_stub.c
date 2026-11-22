/* drr_core.c — DRR Dedicated Recovery Root 核心实现
 *
 * 替换原 drr_stub.c，提供完整的：
 *   - Emergency Pool（独立于普通堆分配器）
 *   - Recovery Log（write intent/commit 追踪）
 *   - Checkpoint A/B 双槽（CRC 校验、原子 active slot 切换）
 *   - Root key 管理
 *   - Heartbeat
 */

#include <utsm/drr.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include <utsm/dma.h>
#include <utsm/segment.h>
#include <utsm/config.h>
#include <utsm/dkm.h>
#include <utsm/crypto.h>
#include <utsm/blake2b.h>
#include <utsm/smp.h>

extern void outb(u16 port, u8 value);

/* ---- 内部内存操作（不依赖 libc） ---- */
static void drr_memzero(void *dst, u64 n) {
    u8 *d = (u8 *)dst;
    while (n--) *d++ = 0;
}

static void drr_memcpy(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
}

/* ---- CRC64-ECMA（CRC-64/XZ：反射多项式 0xC96C5795D7870F42，init/xorout 全 1）
 * KAT：crc64("123456789", 9) == 0x995DC9BBDF1939FA（selftest DRR-0 校验） ---- */
static const u64 crc64_table[256] = {
    0x0000000000000000ULL, 0xB32E4CBE03A75F6FULL, 0xF4843657A840A05BULL, 0x47AA7AE9ABE7FF34ULL,
    0x7BD0C384FF8F5E33ULL, 0xC8FE8F3AFC28015CULL, 0x8F54F5D357CFFE68ULL, 0x3C7AB96D5468A107ULL,
    0xF7A18709FF1EBC66ULL, 0x448FCBB7FCB9E309ULL, 0x0325B15E575E1C3DULL, 0xB00BFDE054F94352ULL,
    0x8C71448D0091E255ULL, 0x3F5F08330336BD3AULL, 0x78F572DAA8D1420EULL, 0xCBDB3E64AB761D61ULL,
    0x7D9BA13851336649ULL, 0xCEB5ED8652943926ULL, 0x891F976FF973C612ULL, 0x3A31DBD1FAD4997DULL,
    0x064B62BCAEBC387AULL, 0xB5652E02AD1B6715ULL, 0xF2CF54EB06FC9821ULL, 0x41E11855055BC74EULL,
    0x8A3A2631AE2DDA2FULL, 0x39146A8FAD8A8540ULL, 0x7EBE1066066D7A74ULL, 0xCD905CD805CA251BULL,
    0xF1EAE5B551A2841CULL, 0x42C4A90B5205DB73ULL, 0x056ED3E2F9E22447ULL, 0xB6409F5CFA457B28ULL,
    0xFB374270A266CC92ULL, 0x48190ECEA1C193FDULL, 0x0FB374270A266CC9ULL, 0xBC9D3899098133A6ULL,
    0x80E781F45DE992A1ULL, 0x33C9CD4A5E4ECDCEULL, 0x7463B7A3F5A932FAULL, 0xC74DFB1DF60E6D95ULL,
    0x0C96C5795D7870F4ULL, 0xBFB889C75EDF2F9BULL, 0xF812F32EF538D0AFULL, 0x4B3CBF90F69F8FC0ULL,
    0x774606FDA2F72EC7ULL, 0xC4684A43A15071A8ULL, 0x83C230AA0AB78E9CULL, 0x30EC7C140910D1F3ULL,
    0x86ACE348F355AADBULL, 0x3582AFF6F0F2F5B4ULL, 0x7228D51F5B150A80ULL, 0xC10699A158B255EFULL,
    0xFD7C20CC0CDAF4E8ULL, 0x4E526C720F7DAB87ULL, 0x09F8169BA49A54B3ULL, 0xBAD65A25A73D0BDCULL,
    0x710D64410C4B16BDULL, 0xC22328FF0FEC49D2ULL, 0x85895216A40BB6E6ULL, 0x36A71EA8A7ACE989ULL,
    0x0ADDA7C5F3C4488EULL, 0xB9F3EB7BF06317E1ULL, 0xFE5991925B84E8D5ULL, 0x4D77DD2C5823B7BAULL,
    0x64B62BCAEBC387A1ULL, 0xD7986774E864D8CEULL, 0x90321D9D438327FAULL, 0x231C512340247895ULL,
    0x1F66E84E144CD992ULL, 0xAC48A4F017EB86FDULL, 0xEBE2DE19BC0C79C9ULL, 0x58CC92A7BFAB26A6ULL,
    0x9317ACC314DD3BC7ULL, 0x2039E07D177A64A8ULL, 0x67939A94BC9D9B9CULL, 0xD4BDD62ABF3AC4F3ULL,
    0xE8C76F47EB5265F4ULL, 0x5BE923F9E8F53A9BULL, 0x1C4359104312C5AFULL, 0xAF6D15AE40B59AC0ULL,
    0x192D8AF2BAF0E1E8ULL, 0xAA03C64CB957BE87ULL, 0xEDA9BCA512B041B3ULL, 0x5E87F01B11171EDCULL,
    0x62FD4976457FBFDBULL, 0xD1D305C846D8E0B4ULL, 0x96797F21ED3F1F80ULL, 0x2557339FEE9840EFULL,
    0xEE8C0DFB45EE5D8EULL, 0x5DA24145464902E1ULL, 0x1A083BACEDAEFDD5ULL, 0xA9267712EE09A2BAULL,
    0x955CCE7FBA6103BDULL, 0x267282C1B9C65CD2ULL, 0x61D8F8281221A3E6ULL, 0xD2F6B4961186FC89ULL,
    0x9F8169BA49A54B33ULL, 0x2CAF25044A02145CULL, 0x6B055FEDE1E5EB68ULL, 0xD82B1353E242B407ULL,
    0xE451AA3EB62A1500ULL, 0x577FE680B58D4A6FULL, 0x10D59C691E6AB55BULL, 0xA3FBD0D71DCDEA34ULL,
    0x6820EEB3B6BBF755ULL, 0xDB0EA20DB51CA83AULL, 0x9CA4D8E41EFB570EULL, 0x2F8A945A1D5C0861ULL,
    0x13F02D374934A966ULL, 0xA0DE61894A93F609ULL, 0xE7741B60E174093DULL, 0x545A57DEE2D35652ULL,
    0xE21AC88218962D7AULL, 0x5134843C1B317215ULL, 0x169EFED5B0D68D21ULL, 0xA5B0B26BB371D24EULL,
    0x99CA0B06E7197349ULL, 0x2AE447B8E4BE2C26ULL, 0x6D4E3D514F59D312ULL, 0xDE6071EF4CFE8C7DULL,
    0x15BB4F8BE788911CULL, 0xA6950335E42FCE73ULL, 0xE13F79DC4FC83147ULL, 0x521135624C6F6E28ULL,
    0x6E6B8C0F1807CF2FULL, 0xDD45C0B11BA09040ULL, 0x9AEFBA58B0476F74ULL, 0x29C1F6E6B3E0301BULL,
    0xC96C5795D7870F42ULL, 0x7A421B2BD420502DULL, 0x3DE861C27FC7AF19ULL, 0x8EC62D7C7C60F076ULL,
    0xB2BC941128085171ULL, 0x0192D8AF2BAF0E1EULL, 0x4638A2468048F12AULL, 0xF516EEF883EFAE45ULL,
    0x3ECDD09C2899B324ULL, 0x8DE39C222B3EEC4BULL, 0xCA49E6CB80D9137FULL, 0x7967AA75837E4C10ULL,
    0x451D1318D716ED17ULL, 0xF6335FA6D4B1B278ULL, 0xB199254F7F564D4CULL, 0x02B769F17CF11223ULL,
    0xB4F7F6AD86B4690BULL, 0x07D9BA1385133664ULL, 0x4073C0FA2EF4C950ULL, 0xF35D8C442D53963FULL,
    0xCF273529793B3738ULL, 0x7C0979977A9C6857ULL, 0x3BA3037ED17B9763ULL, 0x888D4FC0D2DCC80CULL,
    0x435671A479AAD56DULL, 0xF0783D1A7A0D8A02ULL, 0xB7D247F3D1EA7536ULL, 0x04FC0B4DD24D2A59ULL,
    0x3886B22086258B5EULL, 0x8BA8FE9E8582D431ULL, 0xCC0284772E652B05ULL, 0x7F2CC8C92DC2746AULL,
    0x325B15E575E1C3D0ULL, 0x8175595B76469CBFULL, 0xC6DF23B2DDA1638BULL, 0x75F16F0CDE063CE4ULL,
    0x498BD6618A6E9DE3ULL, 0xFAA59ADF89C9C28CULL, 0xBD0FE036222E3DB8ULL, 0x0E21AC88218962D7ULL,
    0xC5FA92EC8AFF7FB6ULL, 0x76D4DE52895820D9ULL, 0x317EA4BB22BFDFEDULL, 0x8250E80521188082ULL,
    0xBE2A516875702185ULL, 0x0D041DD676D77EEAULL, 0x4AAE673FDD3081DEULL, 0xF9802B81DE97DEB1ULL,
    0x4FC0B4DD24D2A599ULL, 0xFCEEF8632775FAF6ULL, 0xBB44828A8C9205C2ULL, 0x086ACE348F355AADULL,
    0x34107759DB5DFBAAULL, 0x873E3BE7D8FAA4C5ULL, 0xC094410E731D5BF1ULL, 0x73BA0DB070BA049EULL,
    0xB86133D4DBCC19FFULL, 0x0B4F7F6AD86B4690ULL, 0x4CE50583738CB9A4ULL, 0xFFCB493D702BE6CBULL,
    0xC3B1F050244347CCULL, 0x709FBCEE27E418A3ULL, 0x3735C6078C03E797ULL, 0x841B8AB98FA4B8F8ULL,
    0xADDA7C5F3C4488E3ULL, 0x1EF430E13FE3D78CULL, 0x595E4A08940428B8ULL, 0xEA7006B697A377D7ULL,
    0xD60ABFDBC3CBD6D0ULL, 0x6524F365C06C89BFULL, 0x228E898C6B8B768BULL, 0x91A0C532682C29E4ULL,
    0x5A7BFB56C35A3485ULL, 0xE955B7E8C0FD6BEAULL, 0xAEFFCD016B1A94DEULL, 0x1DD181BF68BDCBB1ULL,
    0x21AB38D23CD56AB6ULL, 0x9285746C3F7235D9ULL, 0xD52F0E859495CAEDULL, 0x6601423B97329582ULL,
    0xD041DD676D77EEAAULL, 0x636F91D96ED0B1C5ULL, 0x24C5EB30C5374EF1ULL, 0x97EBA78EC690119EULL,
    0xAB911EE392F8B099ULL, 0x18BF525D915FEFF6ULL, 0x5F1528B43AB810C2ULL, 0xEC3B640A391F4FADULL,
    0x27E05A6E926952CCULL, 0x94CE16D091CE0DA3ULL, 0xD3646C393A29F297ULL, 0x604A2087398EADF8ULL,
    0x5C3099EA6DE60CFFULL, 0xEF1ED5546E415390ULL, 0xA8B4AFBDC5A6ACA4ULL, 0x1B9AE303C601F3CBULL,
    0x56ED3E2F9E224471ULL, 0xE5C372919D851B1EULL, 0xA26908783662E42AULL, 0x114744C635C5BB45ULL,
    0x2D3DFDAB61AD1A42ULL, 0x9E13B115620A452DULL, 0xD9B9CBFCC9EDBA19ULL, 0x6A978742CA4AE576ULL,
    0xA14CB926613CF817ULL, 0x1262F598629BA778ULL, 0x55C88F71C97C584CULL, 0xE6E6C3CFCADB0723ULL,
    0xDA9C7AA29EB3A624ULL, 0x69B2361C9D14F94BULL, 0x2E184CF536F3067FULL, 0x9D36004B35545910ULL,
    0x2B769F17CF112238ULL, 0x9858D3A9CCB67D57ULL, 0xDFF2A94067518263ULL, 0x6CDCE5FE64F6DD0CULL,
    0x50A65C93309E7C0BULL, 0xE388102D33392364ULL, 0xA4226AC498DEDC50ULL, 0x170C267A9B79833FULL,
    0xDCD7181E300F9E5EULL, 0x6FF954A033A8C131ULL, 0x28532E49984F3E05ULL, 0x9B7D62F79BE8616AULL,
    0xA707DB9ACF80C06DULL, 0x14299724CC279F02ULL, 0x5383EDCD67C06036ULL, 0xE0ADA17364673F59ULL,
};

u64 drr_crc64(const void *data, u64 len) {
    const u8 *p = (const u8 *)data;
    u64 crc = 0xFFFFFFFFFFFFFFFFULL;
    for (u64 i = 0; i < len; i++) {
        crc = crc64_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFFFFFFFFFULL;
}

/* ---- 全局 DRR 状态 ---- */
static drr_state g_drr;

/* ---- Phase 8: 快照区 / 看门狗 / 恢复行为控制 ---- */
#define DRR_SNAP_MAGIC   0x445252534E415021ULL  /* "DRRSNAP!" */
#define DRR_SNAP_VERSION 1u

typedef struct {
    u64 magic;
    u32 version;
    u32 capacity;     /* 条目容量（page0 空间决定） */
    u32 used;
    u32 generation;
    u64 crc;
} drr_snap_header;

static dkm_dma_buffer g_snap_region;
static int    g_snap_ok = 0;
static u32    g_snap_capacity = 0;
static int    g_reboot_enabled = 1;
static void (*g_sched_fault_cb)(u32 task_slot) = 0;

typedef struct {
    u32 task_slot;        /* 0xFFFFFFFF = 空闲 */
    u32 timeout_ticks;
    u64 last_kick_tick;
} drr_watchdog_entry;
static drr_watchdog_entry g_drr_wd[DRR_WATCHDOG_MAX];

static drr_snap_header *snap_header(void) {
    return (drr_snap_header *)g_snap_region.virt;
}
static drr_ckpt_page_entry *snap_entries(void) {
    return (drr_ckpt_page_entry *)((u8 *)g_snap_region.virt + 64);
}
static u8 *snap_page_ptr(u32 idx) {
    return (u8 *)g_snap_region.virt + 4096ULL * (1ULL + (u64)idx);
}

/* ---- Root key（Phase 9: RDRAND + TSC jitter 混合，boot 生成一次）
 * 生成一次后固化：drr_init 可重入（emergency reset），root key 不换，
 * 否则旧 segment key 全部失效。持久化（sealed disk region）留待后续。 ---- */
static u64 g_root_key_storage[4];
static int  g_root_key_ready = 0;
static const u64 g_root_key_fallback[4] = {
    0x4452525554534D31ULL,  /* "DRRUTSM1" */
    0xFEEDFACECAFEF00DULL,
    0x0123456789ABCDEFULL,
    0xFEDCBA9876543210ULL
};

static u64 drr_tsc(void) {
    u64 v;
    __asm__ volatile("rdtsc" : "=A"(v));
    return v;
}

static int drr_have_rdrand(void) {
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    return (int)((c >> 30) & 1u);
}

static int drr_rdrand64(u64 *out) {
    u8 ok;
    u64 v;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
    if (!ok) return 0;
    *out = v;
    return 1;
}

static void drr_root_key_generate(void) {
    if (g_root_key_ready) {
        g_drr.root_key = g_root_key_storage;
        return;
    }
    int rdrand_ok = 1;
    if (drr_have_rdrand()) {
        for (int i = 0; i < 4; i++) {
            if (!drr_rdrand64(&g_root_key_storage[i])) {
                rdrand_ok = 0;
                break;
            }
        }
    } else {
        rdrand_ok = 0;
    }

    if (rdrand_ok) {
        /* 纵深：混入 TSC jitter，防 RDRAND 单点失效 */
        for (int i = 0; i < 4; i++) {
            g_root_key_storage[i] ^= drr_tsc() * 0x9e3779b97f4a7c15ULL;
        }
    } else {
        /* 无 RDRAND：常量基底 + 多轮 TSC jitter 混合 */
        for (int i = 0; i < 4; i++) {
            g_root_key_storage[i] = g_root_key_fallback[i];
        }
        for (int r = 0; r < 16; r++) {
            u64 j = drr_tsc();
            for (int i = 0; i < 4; i++) {
                j = (j ^ drr_tsc()) * 0x9e3779b97f4a7c15ULL;
                g_root_key_storage[i] ^= j;
                for (volatile int d = 0; d < 50; d++) { }
            }
        }
    }

    g_root_key_ready = 1;
    g_drr.root_key = g_root_key_storage;
    log_info("[DRR] root key generated");
    log_hex64("[DRR]   rdrand=", (u64)rdrand_ok);
}

/* ---- Emergency Pool 内部分配 ---- */
static void *pool_alloc_internal(u64 size, u64 alignment) {
    u64 current = (u64)&g_drr.pool[0] + g_drr.pool_offset;
    u64 aligned = utsm_align_up_u64(current, alignment);
    u64 new_offset = (aligned - (u64)&g_drr.pool[0]) + size;
    if (new_offset > g_drr.pool_limit) {
        return NULL;
    }
    g_drr.pool_offset = new_offset;
    return (void *)aligned;
}

void *drr_emergency_alloc(u64 size, u64 alignment) {
    if (!g_drr.initialized) return NULL;
    return pool_alloc_internal(size, alignment);
}

void drr_emergency_reset(void) {
    if (!g_drr.initialized) return;
    /* 保留已分配的子缓冲区，仅重置池偏移到子缓冲区之后 */
    /* 注意：此函数仅在 recovery 路径紧急情况下使用 */
    g_drr.pool_offset = 0;
    /* 重新分配子缓冲区 */
    drr_init();
}

/* ---- R3b: FUCK 配置接线（drr_apply_config 设置，drr_init 消费） ---- */
static u64 g_cfg_pool_bytes;      /* 0 = 编译期默认 */
static u64 g_cfg_log_size;        /* 0 = 编译期默认 */

void drr_apply_config(u64 pool_kb, u64 recovery_log_size) {
    g_cfg_pool_bytes = pool_kb ? pool_kb * 1024ULL : 0;
    g_cfg_log_size = recovery_log_size;
    if (g_cfg_pool_bytes > DRR_EMERGENCY_POOL_SIZE) {
        log_warn("[DRR] pool_kb exceeds compile-time pool, clamped");
        log_hex64("[DRR]   compile-time=", DRR_EMERGENCY_POOL_SIZE);
        g_cfg_pool_bytes = DRR_EMERGENCY_POOL_SIZE;
    }
}

/* ---- 初始化 ---- */
void drr_init(void) {
    log_info("[DRR] init begin");
    drr_memzero(&g_drr, sizeof(g_drr));

    /* R3b: 池上限 = min(编译期池, FUCK 配置)；小于最小子缓冲布局时回退默认 */
    g_drr.pool_offset = 0;
    g_drr.pool_limit = DRR_EMERGENCY_POOL_SIZE;
    {
        const u64 MIN_LAYOUT =
            DRR_EMERGENCY_STACK_SIZE + DRR_RECOVERY_LOG_SIZE +
            DRR_CKPT_META_BUF_SIZE + DRR_CRASH_BUF_SIZE + DRR_META_SLAB_SIZE;
        if (g_cfg_pool_bytes && g_cfg_pool_bytes >= MIN_LAYOUT) {
            g_drr.pool_limit = g_cfg_pool_bytes;
        } else if (g_cfg_pool_bytes) {
            log_warn("[DRR] pool_kb below minimum sub-buffer layout, default kept");
            log_hex64("[DRR]   minimum=", MIN_LAYOUT);
        }
    }
    drr_root_key_generate();
    g_drr.recovery_generation = 1;
    g_drr.heartbeat_interval = 1000;
    g_drr.active_slot = 0; /* A */

    /* 分配 Emergency Pool 子缓冲区（recovery log 大小可由 FUCK 配置） */
    g_drr.stack_base = (u8 *)pool_alloc_internal(DRR_EMERGENCY_STACK_SIZE, 64);
    g_drr.stack_size = DRR_EMERGENCY_STACK_SIZE;
    g_drr.stack_ptr = DRR_EMERGENCY_STACK_SIZE; /* 栈从顶部向下增长 */

    u64 log_size = DRR_RECOVERY_LOG_SIZE;
    if (g_cfg_log_size && g_cfg_log_size >= sizeof(drr_log_entry) &&
        g_cfg_log_size <= g_drr.pool_limit - g_drr.pool_offset) {
        log_size = g_cfg_log_size;
    } else if (g_cfg_log_size) {
        log_warn("[DRR] recovery_log_size out of range, default kept");
    }
    g_drr.log_base = (drr_log_entry *)pool_alloc_internal(log_size, 64);
    g_drr.log_capacity = (u32)(log_size / sizeof(drr_log_entry));
    g_drr.log_count = 0;

    g_drr.ckpt_meta_buf = (drr_checkpoint_meta *)pool_alloc_internal(DRR_CKPT_META_BUF_SIZE, 64);
    g_drr.crash_buf = (u8 *)pool_alloc_internal(DRR_CRASH_BUF_SIZE, 64);
    g_drr.crash_buf_size = DRR_CRASH_BUF_SIZE;
    g_drr.meta_slab = pool_alloc_internal(DRR_META_SLAB_SIZE, 64);
    g_drr.meta_slab_offset = 0;

    /* 初始化 A/B slot：两槽都未提交（active=0, seq=0）。
     * 首次 checkpoint 之前不存在稳定点，recover 返回 -2 是真实语义。 */
    drr_memzero(&g_drr.ckpt_a, sizeof(drr_checkpoint_meta));
    drr_memzero(&g_drr.ckpt_b, sizeof(drr_checkpoint_meta));
    g_drr.ckpt_a.magic = DRR_CKPT_MAGIC;
    g_drr.ckpt_a.version = DRR_CKPT_VERSION;
    g_drr.ckpt_b.magic = DRR_CKPT_MAGIC;
    g_drr.ckpt_b.version = DRR_CKPT_VERSION;
    g_drr.ckpt_seq = 0;
    g_drr.ckpt_in_progress = 0;
    g_drr.ckpt_target = 0;

    g_drr.initialized = 1;
    log_hex64("[DRR]   pool_limit=", g_drr.pool_limit);
    log_hex64("[DRR]   log_capacity=", (u64)g_drr.log_capacity);

    /* ---- Phase 8: 快照区（dma 页，独立于普通堆；失败降级 metadata-only） ---- */
    g_snap_ok = 0;
    g_snap_capacity = 0;
    g_reboot_enabled = 1;
    g_sched_fault_cb = 0;
    for (int wdi = 0; wdi < DRR_WATCHDOG_MAX; wdi++) {
        g_drr_wd[wdi].task_slot = 0xFFFFFFFFu;
        g_drr_wd[wdi].timeout_ticks = 0;
        g_drr_wd[wdi].last_kick_tick = 0;
    }
    {
        /* page0 = 头 + 条目数组，其后 DRR_SNAP_DATA_PAGES 页快照数据 */
        dkm_dma_buffer buf;
        if (dma_alloc_pages(1 + DRR_SNAP_DATA_PAGES, 4096, 0xFFFFFFFFULL, &buf) == 0 && buf.virt) {
            g_snap_region = buf;
            u32 cap = (u32)((4096 - 64) / sizeof(drr_ckpt_page_entry));
            drr_snap_header *h = (drr_snap_header *)buf.virt;
            drr_memzero(h, 4096);
            h->magic = DRR_SNAP_MAGIC;
            h->version = DRR_SNAP_VERSION;
            h->capacity = cap;
            h->used = 0;
            h->generation = 0;
            h->crc = drr_crc64(h, __builtin_offsetof(drr_snap_header, crc));
            g_snap_capacity = cap;
            g_snap_ok = 1;
            log_info("[DRR] snapshot region ok");
            log_hex64("[DRR]   pages=", 1 + DRR_SNAP_DATA_PAGES);
        } else {
            /* 严格错误策略插桩：快照区是 DRR 页回滚的前提，缺失即降级
             * metadata-only——按零降级原则直接 panic（DRR-E01）。 */
            panic_full("DRR-E01 SNAPSHOT REGION ALLOC FAILED",
                       "snapshot dma pages unavailable; metadata-only mode is forbidden", 0);
        }
    }

    log_info("[DRR] init ok");
}

/* ===================================================================
 *  严格错误策略插桩：设计已定但未实现的 DRR 能力，建立代码骨架，
 *  调用即 panic（DRR-E2x 系列）。当前无调用方（骨架）。
 * =================================================================== */

/* Root key 持久化（sealed disk region）：drr_init 可重入要求 root key
 * 跨 emergency reset 固化。当前实现走内存 fallback key（见
 * drr_root_key_generate），sealed disk 尚未落盘。 */
void drr_root_key_seal_to_disk(void) {
    panic_full("DRR-E02 ROOT KEY SEAL NOT IMPLEMENTED",
               "sealed disk region for root key persistence: designed, not implemented", 0);
}

/* task_kill crypto erase：任务被杀时 key_epoch++ 并擦除其密钥材料。
 * 当前 task_exit 只回收 slot，不做 crypto erase。 */
void drr_task_kill_crypto_erase(u32 task_slot) {
    (void)task_slot;
    panic_full("DRR-E03 TASK KILL CRYPTO ERASE NOT IMPLEMENTED",
               "key_epoch++ erase on task kill: designed, not implemented", 0);
}

/* segment/system 级回滚（R4）已实现，见文件尾部 Phase 8 扩展区。
 * 原骨架 panic（DRR-E04/E05 NOT IMPLEMENTED）由真实现取代：
 *   - DRR-E04 现语义：segment 回滚的结构前提不成立（无快照区/无
 *     DIRTY_PAGE checkpoint）即 panic。
 *   - DRR-E05 现语义：system 回滚升级 reboot 前的留痕日志。 */

/* DKM 驱动 recovery ops（DKM README 设计：quiesce/reset/reinit/dump_state）。
 * 驱动侧从未提供该回调表，DRR 亦未在恢复路径调用。 */
typedef struct {
    u32 magic;          /* 'D','R','O','P' */
    u32 version;
    void (*quiesce)(void *drv);
    void (*reset)(void *drv);
    void (*reinit)(void *drv);
    void (*dump_state)(void *drv);
} dkm_driver_recovery_ops;

void drr_driver_recovery_dispatch(const dkm_driver_recovery_ops *ops, void *drv) {
    (void)drv;
    if (!ops) {
        panic_full("DRR-E06 DRIVER RECOVERY OPS MISSING",
                   "driver did not provide dkm_driver_recovery_ops", 0);
        return;
    }
    panic_full("DRR-E07 DRIVER RECOVERY NOT IMPLEMENTED",
               "quiesce/reset/reinit dispatch: designed (DKM README), not implemented", 0);
}

const u64 *drr_get_root_key(void) {
    return g_drr.root_key;
}

u64 drr_recovery_generation(void) {
    return g_drr.recovery_generation;
}

/* ---- Fault 报告 ---- */
void drr_report_fault(const char *reason) {
    if (!g_drr.initialized) return;
    log_error("[DRR] FAULT: ");
    log_error(reason);
    /* 记录到 crash buffer */
    if (g_drr.crash_buf && g_drr.crash_buf_size > 0) {
        /* 简单记录：写入 reason 的前 N 字节 */
        u64 i = 0;
        const char *s = reason;
        while (*s && i < g_drr.crash_buf_size - 1) {
            g_drr.crash_buf[i++] = (u8)*s++;
        }
        g_drr.crash_buf[i] = 0;
    }
    /* 增加 recovery generation */
    g_drr.recovery_generation++;
}

/* ---- F1: 崩溃归档（panic_full 调用；crash_buf 写 symbol+msg+rip 简表） ----
 * 返回 0=已归档；-1=DRR 未初始化（crash_buf 不可用）。 */
int drr_crash_archive(const char *symbol, const char *msg, const void *regs) {
    if (!g_drr.initialized || !g_drr.crash_buf || g_drr.crash_buf_size == 0) {
        return -1;
    }
    u64 i = 0;
    u8 *b = g_drr.crash_buf;
    u64 cap = g_drr.crash_buf_size - 1;
    /* 头部标记 */
    const char *magic = "CRASH1";
    for (const char *s = magic; *s && i < cap; s++) b[i++] = (u8)*s;
    b[i++] = '|';
    for (const char *s = symbol ? symbol : "UNKNOWN"; *s && i < cap; s++) b[i++] = (u8)*s;
    b[i++] = '|';
    for (const char *s = msg ? msg : "-"; *s && i < cap; s++) b[i++] = (u8)*s;
    b[i] = 0;
    /* recovery generation 前进（归档计数） */
    g_drr.recovery_generation++;
    log_error("[DRR] crash archived");
    log_hex64("[DRR]   generation=", g_drr.recovery_generation);
    return 0;
}

/* ---- Recovery Log ---- */
void drr_log_write_intent(u32 segment_slot, u64 offset, u64 len) {
    if (!g_drr.initialized || !g_drr.log_base) return;
    if (g_drr.log_count >= g_drr.log_capacity) {
        /* 日志满，最简单的策略：丢弃最早的条目 */
        for (u32 i = 1; i < g_drr.log_count; i++) {
            g_drr.log_base[i - 1] = g_drr.log_base[i];
        }
        g_drr.log_count--;
    }
    drr_log_entry *e = &g_drr.log_base[g_drr.log_count++];
    e->segment_slot = segment_slot;
    e->flags = DRR_LOG_FLAG_INTENT;
    e->offset = offset;
    e->length = len;
    e->timestamp = g_drr.heartbeat_counter;
}

void drr_log_write_commit(u32 segment_slot, u64 offset, u64 len) {
    if (!g_drr.initialized || !g_drr.log_base) return;
    if (g_drr.log_count >= g_drr.log_capacity) {
        for (u32 i = 1; i < g_drr.log_count; i++) {
            g_drr.log_base[i - 1] = g_drr.log_base[i];
        }
        g_drr.log_count--;
    }
    drr_log_entry *e = &g_drr.log_base[g_drr.log_count++];
    e->segment_slot = segment_slot;
    e->flags = DRR_LOG_FLAG_COMMIT;
    e->offset = offset;
    e->length = len;
    e->timestamp = g_drr.heartbeat_counter;
}

/* ---- Checkpoint ---- */
static drr_checkpoint_meta *ckpt_slot(u32 slot) {
    return slot == 0 ? &g_drr.ckpt_a : &g_drr.ckpt_b;
}

static drr_checkpoint_meta *ckpt_target(void) {
    return ckpt_slot(g_drr.ckpt_target);
}

int drr_checkpoint_begin(drr_checkpoint_type type) {
    if (!g_drr.initialized) return -1;

    /* 目标 = 非 active 槽（提交成功后才翻转 active_slot） */
    u32 target_slot = g_drr.active_slot ? 0u : 1u;
    drr_checkpoint_meta *target = ckpt_slot(target_slot);

    target->magic = DRR_CKPT_MAGIC;
    target->version = DRR_CKPT_VERSION;
    target->active = 0; /* 写入时为 preparing */
    target->type = (u32)type;
    target->reserved0 = 0;
    target->seq = 0;
    target->global_epoch = g_drr.recovery_generation;
    target->snap_gen = 0;
    target->snap_crc = 0;
    target->dirty_page_count = 0;
    target->mac_root = 0;
    target->segment_count = 0;
    target->crc = 0;

    g_drr.ckpt_target = target_slot;
    g_drr.ckpt_in_progress = 1;
    return 0;
}

int drr_checkpoint_finish(void) {
    if (!g_drr.initialized) return -1;
    if (!g_drr.ckpt_in_progress) return -1;   /* 没有 begin 的 finish 不提交 */

    drr_checkpoint_meta *target = ckpt_target();
    u32 target_slot = g_drr.ckpt_target;

    /* 提交字顺序：active → seq → CRC（最后一条写）。
     * 任何中断点都至少保留一个 CRC 合法的槽：旧槽保持已提交态，新槽在
     * CRC 写入前不可通过 ckpt_slot_valid。 */
    target->active = 1;
    target->seq = ++g_drr.ckpt_seq;
    target->crc = drr_crc64(target, DRR_CKPT_CRC_LEN);

    /* 旧槽保留 active=1 作为 fallback（提交位语义 = 已提交，不代表「当前」）；
     * active_slot 只表示最近一次提交，恢复仍按 seq 取最新。 */
    g_drr.active_slot = target_slot;
    g_drr.ckpt_in_progress = 0;

    /* 增加 recovery generation */
    g_drr.recovery_generation++;

    /* 清空 recovery log */
    g_drr.log_count = 0;

    return 0;
}

/* 槽位有效性 = magic + version + active + seq!=0 + CRC 全通过。
 * CRC 覆盖 [0, offsetof(crc))，即提交后的最终字节（active/seq 均已写入）。 */
static int ckpt_slot_valid(const drr_checkpoint_meta *s) {
    if (s->magic != DRR_CKPT_MAGIC || s->version != DRR_CKPT_VERSION) return 0;
    if (s->active != 1 || s->seq == 0) return 0;
    u64 expected_crc = s->crc;
    drr_checkpoint_meta tmp = *s;
    tmp.crc = 0;
    return drr_crc64(&tmp, DRR_CKPT_CRC_LEN) == expected_crc;
}

int drr_checkpoint_recover(drr_checkpoint_meta *out_meta) {
    if (!g_drr.initialized || !out_meta) return -1;

    const drr_checkpoint_meta *slot_a = &g_drr.ckpt_a;
    const drr_checkpoint_meta *slot_b = &g_drr.ckpt_b;
    int valid_a = ckpt_slot_valid(slot_a);
    int valid_b = ckpt_slot_valid(slot_b);

    /* 双槽都无效 */
    if (!valid_a && !valid_b) return -2;

    /* 合法槽中取 seq 最大者；seq 相同优先 RAM active_slot */
    const drr_checkpoint_meta *chosen;
    u32 chosen_slot;
    if (valid_a && valid_b) {
        if (slot_a->seq > slot_b->seq) {
            chosen = slot_a;
            chosen_slot = 0;
        } else if (slot_b->seq > slot_a->seq) {
            chosen = slot_b;
            chosen_slot = 1;
        } else {
            chosen_slot = g_drr.active_slot ? 1u : 0u;
            chosen = ckpt_slot(chosen_slot);
        }
    } else if (valid_a) {
        chosen = slot_a;
        chosen_slot = 0;
    } else {
        chosen = slot_b;
        chosen_slot = 1;
    }

    if (chosen_slot != g_drr.active_slot) {
        log_warn("[DRR] recover fallback slot");
        log_hex64("[DRR]   fallback_slot=", chosen_slot);
        /* RAM 指针与真实最新稳定点对齐，后续 checkpoint 写另一槽 */
        g_drr.active_slot = chosen_slot;
    }

    drr_memcpy(out_meta, chosen, sizeof(drr_checkpoint_meta));
    return 0;
}

/* ---- Heartbeat ---- */
void drr_heartbeat_tick(void) {
    if (!g_drr.initialized) return;
    g_drr.heartbeat_counter++;
}

u64 drr_heartbeat_get(void) {
    return g_drr.heartbeat_counter;
}

/* ===================================================================
 *  Phase 8: 真实脏页快照 / 页级回滚 / 看门狗 / system rollback
 * =================================================================== */

int drr_ckpt_snapshot_dirty(void) {
    if (!g_drr.initialized) return -1;
    if (!g_snap_ok) return -2;   /* metadata-only 模式不支持页快照 */

    if (drr_checkpoint_begin(DRR_CKPT_DIRTY_PAGE) != 0) return -1;
    drr_snap_header *h = snap_header();
    drr_ckpt_page_entry *entries = snap_entries();
    u32 used = 0;

    for (u32 slot = 0; slot < g_utsm_max_segments; slot++) {
        utsm_segment_desc *desc = utsm_get_segment(slot);
        if (!desc || desc->state != UTSM_SEG_ACTIVE) continue;
        desc->state = UTSM_SEG_CHECKPOINTING;

        for (u32 s = 0; s < desc->dirty_shard_count && desc->dirty_shards; s++) {
            utsm_dirty_shard *shard = &desc->dirty_shards[s];
            if (!shard->bitmap) continue;
            u64 words = (shard->page_count + 63) / 64;
            for (u64 w = 0; w < words && used < g_snap_capacity; w++) {
                u64 word = shard->bitmap[w];
                while (word && used < g_snap_capacity) {
                    u64 bit = (u64)__builtin_ctzll(word);
                    u64 local_page = w * 64 + bit;
                    u64 page = shard->page_base + local_page;   /* 段内页号 */
                    if (local_page < shard->page_count) {
                        u8 *src = desc->cipher_base + page * UTSM_PAGE_SIZE;
                        u8 *dst = snap_page_ptr(used);
                        drr_memcpy(dst, src, UTSM_PAGE_SIZE);
                        entries[used].seg_slot = slot;
                        entries[used].page_index = (u32)page;
                        entries[used].cipher_vaddr = (u64)src;
                        entries[used].crc = drr_crc64(dst, UTSM_PAGE_SIZE);
                        entries[used].key_epoch = desc->key_epoch;
                        entries[used].mac = utsm_page_mac(g_drr.root_key, slot, page,
                                                          desc->key_epoch, dst);
                        entries[used].region_idx = used;
                        entries[used].used = 1;
                        used++;
                    }
                    /* 快照成功才清脏位；区满时保留脏位（部分提交，下轮重试） */
                    shard->bitmap[w] &= ~(1ULL << bit);
                    if (shard->dirty_count > 0) shard->dirty_count--;
                    word &= ~(1ULL << bit);
                }
            }
            shard->last_checkpoint_epoch = g_drr.recovery_generation;
        }
        desc->state = UTSM_SEG_ACTIVE;
        desc->checkpoint_generation = g_drr.recovery_generation;
        if (used >= g_snap_capacity) break;
    }

    /* 区域头（crc 不含自身字段），先定稿数据区再提交元数据 */
    h->used = used;
    h->generation = (u32)g_drr.recovery_generation;
    h->crc = drr_crc64(h, __builtin_offsetof(drr_snap_header, crc));

    /* 元数据 + A/B 原子切换（只认 begin 选中的槽，不再二次推导） */
    drr_checkpoint_meta *target = ckpt_target();
    target->dirty_page_count = used;
    target->snap_gen = (u64)h->generation;
    target->snap_crc = h->crc;
    /* Phase 9: mac_root = keyed BLAKE2b 链（root_key 派生，绑定全部页 MAC） */
    {
        u8 kb[32];
        u8 chain[64];
        for (int i = 0; i < 4; i++) utsm_store64_le(kb + 8 * i, g_drr.root_key[i]);
        utsm_blake2b_ctx mc;
        utsm_blake2b_init(&mc, 64, kb, 32);
        for (u32 i = 0; i < used; i++) {
            u8 rec[8 + 8 + 8 + 8];
            utsm_store64_le(rec + 0, 0x544F4F524D535455ULL);    /* "UTSMROOT" LE */
            utsm_store64_le(rec + 8, ((u64)entries[i].seg_slot) |
                                     ((u64)entries[i].page_index << 32));
            utsm_store64_le(rec + 16, entries[i].key_epoch);
            utsm_store64_le(rec + 24, entries[i].mac);
            utsm_blake2b_update(&mc, rec, sizeof(rec));
        }
        utsm_blake2b_final(&mc, chain);
        target->mac_root = utsm_load64_le(chain);
    }
    target->segment_count = 0;
    if (drr_checkpoint_finish() != 0) {
        log_error("[DRR] checkpoint finish failed");
        return -2;
    }

    log_info("[DRR] ckpt snapshot ok");
    log_hex64("[DRR]   pages=", used);
    return (int)used;
}

/* 槽元数据与快照区一致性：区域头自校验 + generation/crc 双绑定。
 * 单快照区下，槽与数据区必须同代际才允许回滚（跨代际旧数据稳定点需要
 * 双 bank，属后续项）；不一致时调用方按各自策略拒绝。 */
static int snap_region_matches(const drr_checkpoint_meta *m) {
    if (!m || !g_snap_ok) return 0;
    drr_snap_header *h = snap_header();
    if (h->magic != DRR_SNAP_MAGIC || h->version != DRR_SNAP_VERSION) return 0;
    if (h->used > h->capacity || h->capacity > g_snap_capacity) return 0;
    if (drr_crc64(h, __builtin_offsetof(drr_snap_header, crc)) != h->crc) return 0;
    return m->snap_gen == (u64)h->generation && m->snap_crc == h->crc;
}

/* 条目校验失败：段隔离（POISONED）+ 日志，不写内存 */
static void rollback_fail_entry(const drr_ckpt_page_entry *e,
                                utsm_segment_desc *d, const char *what) {
    if (d) d->state = UTSM_SEG_POISONED;
    log_error(what);
    log_hex64("[DRR]   seg_slot=", e->seg_slot);
    log_hex64("[DRR]   page=", e->page_index);
}

/* R4: 共享恢复核心。seg_slot_filter == DRR_ROLLBACK_ALL_SEGMENTS 表示
 * 全部段（system 级），否则只恢复该段的快照页。
 * 前置：caller 已完成 initialized/snap_ok/meta/代际校验。
 * 返回恢复页数；*out_failed 置 1 表示任一页校验失败（该段已 POISONED，
 * 与页级路径策略一致：数据损坏不 panic，段隔离）。 */
#define DRR_ROLLBACK_ALL_SEGMENTS 0xFFFFFFFFu
static int rollback_entries(u32 seg_slot_filter, int *out_failed) {
    drr_snap_header *h = snap_header();
    if (h->magic != DRR_SNAP_MAGIC || h->version != DRR_SNAP_VERSION ||
        h->used > h->capacity || h->capacity > g_snap_capacity ||
        drr_crc64(h, __builtin_offsetof(drr_snap_header, crc)) != h->crc) {
        *out_failed = 1;
        return 0;
    }
    u32 used = h->used;

    drr_ckpt_page_entry *entries = snap_entries();

    /* 触碰段先置 RECOVERING */
    for (u32 i = 0; i < used; i++) {
        if (seg_slot_filter != DRR_ROLLBACK_ALL_SEGMENTS &&
            entries[i].seg_slot != seg_slot_filter) continue;
        utsm_segment_desc *d = utsm_get_segment(entries[i].seg_slot);
        if (d && d->state == UTSM_SEG_ACTIVE) d->state = UTSM_SEG_RECOVERING;
    }

    *out_failed = 0;
    u32 restored = 0;
    for (u32 i = 0; i < used; i++) {
        drr_ckpt_page_entry *e = &entries[i];
        if (seg_slot_filter != DRR_ROLLBACK_ALL_SEGMENTS &&
            e->seg_slot != seg_slot_filter) continue;
        utsm_segment_desc *d = utsm_get_segment(e->seg_slot);

        /* 条目结构校验：used 标记 + 区域页号上界 + 段内页号 + 目标地址重算 */
        if (e->used != 1 || e->region_idx >= DRR_SNAP_DATA_PAGES) {
            *out_failed = 1;
            rollback_fail_entry(e, d, "[DRR] rollback entry invalid");
            continue;
        }
        u8 *snap = snap_page_ptr(e->region_idx);
        if (!d) {
            *out_failed = 1;
            rollback_fail_entry(e, d, "[DRR] rollback segment missing");
            continue;
        }
        u64 seg_pages = d->cipher_length / UTSM_PAGE_SIZE;
        if ((u64)e->page_index >= seg_pages) {
            *out_failed = 1;
            rollback_fail_entry(e, d, "[DRR] rollback page out of segment");
            continue;
        }
        u8 *dst = d->cipher_base + (u64)e->page_index * UTSM_PAGE_SIZE;
        if ((u64)dst != e->cipher_vaddr) {
            *out_failed = 1;
            rollback_fail_entry(e, d, "[DRR] rollback target vaddr mismatch");
            continue;
        }

        if (drr_crc64(snap, UTSM_PAGE_SIZE) != e->crc) {
            *out_failed = 1;
            rollback_fail_entry(e, d, "[DRR] rollback crc fail");
            continue;
        }
        /* Phase 9: 页 MAC 校验（伪造/跨纪元重放/区域篡改在此被拒） */
        u64 mac = utsm_page_mac(g_drr.root_key, e->seg_slot, e->page_index,
                                e->key_epoch, snap);
        if (mac != e->mac) {
            *out_failed = 1;
            rollback_fail_entry(e, d, "[DRR] rollback mac fail");
            continue;
        }
        drr_memcpy(dst, snap, UTSM_PAGE_SIZE);
        restored++;
        /* 清除该页脏位（P2 之类的回写不再被误判为未 checkpoint）：
         * 按 page_base 在全部 shard 中定位，而不是只认 shards[0]。 */
        if (d->dirty_shards && d->dirty_shard_count > 0) {
            u64 page = e->page_index;
            for (u32 si = 0; si < d->dirty_shard_count; si++) {
                utsm_dirty_shard *sh = &d->dirty_shards[si];
                if (!sh->bitmap) continue;
                if (page < sh->page_base || page >= sh->page_base + sh->page_count) continue;
                u64 local = page - sh->page_base;
                if (sh->bitmap[local / 64] & (1ULL << (local % 64))) {
                    sh->bitmap[local / 64] &= ~(1ULL << (local % 64));
                    if (sh->dirty_count > 0) sh->dirty_count--;
                }
                break;
            }
        }
        if (d->state == UTSM_SEG_RECOVERING) d->state = UTSM_SEG_ACTIVE;
    }
    return (int)restored;
}

int drr_rollback_pages(void) {
    if (!g_drr.initialized) return -1;
    if (!g_snap_ok) return -2;
    drr_checkpoint_meta meta;
    if (drr_checkpoint_recover(&meta) != 0) return -3;
    if (meta.type != DRR_CKPT_DIRTY_PAGE) return -4;
    if (!snap_region_matches(&meta)) {
        log_error("[DRR] snapshot generation mismatch");
        log_hex64("[DRR]   meta_snap_gen=", meta.snap_gen);
        log_hex64("[DRR]   region_gen=", (u64)snap_header()->generation);
        return -2;
    }

    int failed = 0;
    int restored = rollback_entries(DRR_ROLLBACK_ALL_SEGMENTS, &failed);
    if (failed) return -2;

    log_info("[DRR] rollback done");
    log_hex64("[DRR]   pages=", (u64)restored);
    return restored;
}

/* ---- R4: segment 级回滚 ---- */
int drr_rollback_segment(u32 seg_slot) {
    if (!g_drr.initialized || !g_snap_ok) {
        panic_full("DRR-E04 SEGMENT ROLLBACK UNAVAILABLE",
                   "segment rollback requires initialized DRR with snapshot region", 0);
    }
    drr_checkpoint_meta meta;
    if (drr_checkpoint_recover(&meta) != 0) {
        /* 设计 §8.2：双槽都无效 → system rollback */
        drr_system_rollback("segment rollback: both slots invalid");
    }
    if (meta.type != DRR_CKPT_DIRTY_PAGE) {
        panic_full("DRR-E04 SEGMENT ROLLBACK UNAVAILABLE",
                   "no dirty-page checkpoint: no ciphertext snapshot to restore", 0);
    }
    if (!snap_region_matches(&meta)) {
        utsm_segment_desc *d = utsm_get_segment(seg_slot);
        if (d) d->state = UTSM_SEG_POISONED;
        log_error("[DRR] snapshot generation mismatch");
        log_hex64("[DRR]   meta_snap_gen=", meta.snap_gen);
        log_hex64("[DRR]   region_gen=", (u64)snap_header()->generation);
        log_hex64("[DRR]   seg_slot=", seg_slot);
        return -2;
    }

    /* 快照区无该段条目：无法恢复 → 段 POISONED，调用方决定升级 */
    drr_snap_header *h = snap_header();
    u32 used = h->used;
    if (used > h->capacity) used = h->capacity;
    drr_ckpt_page_entry *entries = snap_entries();
    int found = 0;
    for (u32 i = 0; i < used; i++) {
        if (entries[i].seg_slot == seg_slot) { found = 1; break; }
    }
    if (!found) {
        utsm_segment_desc *d = utsm_get_segment(seg_slot);
        if (d) d->state = UTSM_SEG_POISONED;
        log_error("[DRR] segment rollback: no snapshot entries");
        log_hex64("[DRR]   seg_slot=", seg_slot);
        return -3;
    }

    int failed = 0;
    int restored = rollback_entries(seg_slot, &failed);
    if (failed) {
        log_error("[DRR] segment rollback verify failed");
        log_hex64("[DRR]   seg_slot=", seg_slot);
        return -2;
    }
    log_info("[DRR] segment rollback ok");
    log_hex64("[DRR]   seg_slot=", seg_slot);
    log_hex64("[DRR]   pages=", (u64)restored);
    return restored;
}

/* ---- R4: system 级回滚（内存内全量恢复；升级路径不返回） ---- */
int drr_rollback_system(void) {
    if (!g_drr.initialized || !g_snap_ok) {
        panic_full("DRR-E05 SYSTEM ROLLBACK UNAVAILABLE",
                   "system rollback requires initialized DRR with snapshot region", 0);
    }
    drr_checkpoint_meta meta;
    if (drr_checkpoint_recover(&meta) != 0) {
        /* 设计 §8.2：双槽都无效 → system rollback（8042 复位，不返回） */
        log_error("[DRR-E05 SYSTEM ROLLBACK ESCALATED]");
        drr_system_rollback("system rollback: both slots invalid");
    }
    if (meta.type != DRR_CKPT_DIRTY_PAGE) {
        log_error("[DRR-E05 SYSTEM ROLLBACK ESCALATED]");
        drr_system_rollback("system rollback: no dirty-page checkpoint");
    }
    if (!snap_region_matches(&meta)) {
        log_error("[DRR] snapshot generation mismatch");
        log_hex64("[DRR]   meta_snap_gen=", meta.snap_gen);
        log_hex64("[DRR]   region_gen=", (u64)snap_header()->generation);
        if (!g_reboot_enabled) {
            /* selftest/dry-run：拒绝不一致恢复，不写任何页 */
            return -5;
        }
        log_error("[DRR-E05 SYSTEM ROLLBACK ESCALATED]");
        drr_system_rollback("system rollback: snapshot generation mismatch");
        return -5;
    }

    int failed = 0;
    int restored = rollback_entries(DRR_ROLLBACK_ALL_SEGMENTS, &failed);
    if (failed) {
        log_error("[DRR-E05 SYSTEM ROLLBACK ESCALATED]");
        drr_system_rollback("system rollback: page verify failed");
    }
    log_info("[DRR] system rollback ok");
    log_hex64("[DRR]   pages=", (u64)restored);
    return restored;
}

/* ---- R4: selftest 专用钩子（非生产路径） ---- */
void drr_test_corrupt_slot(u32 slot) {
    if (slot == 0) {
        g_drr.ckpt_a.crc ^= 1;
    } else if (slot == 1) {
        g_drr.ckpt_b.crc ^= 1;
    }
}

u64 drr_test_slot_seq(u32 slot) {
    if (slot > 1) return 0;
    return ckpt_slot(slot)->seq;
}

u64 drr_test_region_gen(void) {
    if (!g_snap_ok) return 0;
    return (u64)snap_header()->generation;
}

void drr_test_bump_region_gen(void) {
    if (!g_snap_ok) return;
    drr_snap_header *h = snap_header();
    h->generation++;
    h->crc = drr_crc64(h, __builtin_offsetof(drr_snap_header, crc));
}

u32 drr_test_active_slot(void) {
    return g_drr.active_slot;
}

void drr_system_rollback(const char *reason) {
    log_error("[DRR] SYSTEM ROLLBACK");
    if (reason) log_error(reason);
    log_hex64("[DRR]   generation=", g_drr.recovery_generation);
    log_hex64("[DRR]   heartbeat=", g_drr.heartbeat_counter);
    log_error("[DRR] rebooting (8042 reset)");
    smp_halt_others();      /* R5：复位前停住其它 CPU */
    for (;;) {
        __asm__ volatile("cli");
        outb(0x64, 0xFE);   /* 8042 复位（A20 gate 已无关紧要） */
        for (volatile u32 i = 0; i < 1000000u; i++) { }
        __asm__ volatile("hlt");
    }
}

void drr_handle_task_fault(u32 task_slot, const char *reason) {
    log_error("[DRR] FAULT");
    if (reason) log_error(reason);
    log_hex64("[DRR]   task_slot=", task_slot);
    drr_report_fault(reason ? reason : "task fault");
    drr_log_write_intent(task_slot, 0, 0);
    int pages = drr_rollback_pages();
    log_hex64("[DRR]   rollback_pages=", (u64)(i64)pages);
    if (g_sched_fault_cb) {
        g_sched_fault_cb(task_slot);      /* 可能不返回（当前任务被杀+切换） */
        drr_log_write_commit(task_slot, 0, 0);
        return;
    }
    if (g_reboot_enabled) {
        drr_system_rollback(reason ? reason : "task fault (no sched cb)");
    }
    /* reboot disabled（selftest）：仅留日志 */
}

int drr_watchdog_register(u32 task_slot, u32 timeout_ticks) {
    if (!g_drr.initialized || timeout_ticks == 0) return -1;
    for (int i = 0; i < DRR_WATCHDOG_MAX; i++) {
        if (g_drr_wd[i].task_slot == task_slot) {
            g_drr_wd[i].timeout_ticks = timeout_ticks;
            g_drr_wd[i].last_kick_tick = g_drr.heartbeat_counter;
            return 0;
        }
    }
    for (int i = 0; i < DRR_WATCHDOG_MAX; i++) {
        if (g_drr_wd[i].task_slot == 0xFFFFFFFFu) {
            g_drr_wd[i].task_slot = task_slot;
            g_drr_wd[i].timeout_ticks = timeout_ticks;
            g_drr_wd[i].last_kick_tick = g_drr.heartbeat_counter;
            return 0;
        }
    }
    return -2;
}

int drr_watchdog_unregister(u32 task_slot) {
    for (int i = 0; i < DRR_WATCHDOG_MAX; i++) {
        if (g_drr_wd[i].task_slot == task_slot) {
            g_drr_wd[i].task_slot = 0xFFFFFFFFu;
            return 0;
        }
    }
    return -1;
}

void drr_watchdog_kick(u32 task_slot) {
    for (int i = 0; i < DRR_WATCHDOG_MAX; i++) {
        if (g_drr_wd[i].task_slot == task_slot) {
            g_drr_wd[i].last_kick_tick = g_drr.heartbeat_counter;
            return;
        }
    }
}

void drr_watchdog_check(void) {
    if (!g_drr.initialized) return;
    for (int i = 0; i < DRR_WATCHDOG_MAX; i++) {
        u32 slot = g_drr_wd[i].task_slot;
        if (slot == 0xFFFFFFFFu) continue;
        u64 now = g_drr.heartbeat_counter;
        if (now - g_drr_wd[i].last_kick_tick > g_drr_wd[i].timeout_ticks) {
            g_drr_wd[i].last_kick_tick = now;   /* 防重复触发 */
            drr_handle_task_fault(slot, "watchdog timeout");
        }
    }
}

void drr_set_reboot_enabled(int enabled) {
    g_reboot_enabled = enabled;
}

void drr_set_sched_fault_cb(void (*cb)(u32 task_slot)) {
    g_sched_fault_cb = cb;
}

/* kernel_api.drr 服务表（dkm_fill_platform_info 挂入） */
static const drr_recovery_api g_drr_api = {
    .report_fault = drr_report_fault,
    .heartbeat_get = drr_heartbeat_get,
    .emergency_alloc = drr_emergency_alloc,
    .ckpt_snapshot_dirty = drr_ckpt_snapshot_dirty,
    .rollback_pages = drr_rollback_pages,
    .rollback_segment = drr_rollback_segment,   /* R4 尾部追加 */
    .rollback_system = drr_rollback_system      /* R4 尾部追加 */
};

const drr_recovery_api *drr_get_recovery_api(void) {
    return &g_drr_api;
}
