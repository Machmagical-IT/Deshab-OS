#include <utsm/log.h>
#include <utsm/spinlock.h>

static utsm_spinlock g_log_lock;

/* ---- 内存日志缓冲区（实机启动日志持久化） ----
 * 默认启用捕获：所有日志同时写串口和内存缓冲。
 * 块设备就绪后由 disk_log_flush() 把缓冲区写入 FAT32 文件
 * SYSTEM/DESHAB64/DEV/BOOTLOG.TXT（DEV 缺失时回退根目录 BOOTLOG.TXT）。 */
#define LOG_BUF_SIZE 524288u  /* 512KB, 覆盖 LBA 2-1000 (999 扇区 ~500KB) */
static char g_log_buf[LOG_BUF_SIZE];
static u32  g_log_len = 0;
static int  g_log_capture_on = 1;

void log_capture_enable(int enable) {
    g_log_capture_on = enable;
}

const char *log_get_buffer(void) {
    return g_log_buf;
}

u32 log_get_length(void) {
    return g_log_len;
}

static void log_emit_raw(const char *s) {
    serial_write(s);
    if (g_log_capture_on && s) {
        while (*s) {
            if (g_log_len < LOG_BUF_SIZE) {
                g_log_buf[g_log_len++] = *s;
            }
            s++;
        }
    }
}

/* ---- D4: DKM 彩色日志 ----
 * 驱动 init 期间由 dkm_log_set_driver() 设置 ANSI 颜色上下文；
 * 颜色只走串口（QEMU stdio 可见），BOOTLOG 缓冲保持纯文本。 */
static const char *g_log_color = 0;
static int g_log_color_on = 1;

void log_set_color(const char *ansi) {
    g_log_color = ansi;
}

void log_color_enable(int enable) {
    g_log_color_on = enable;
    if (!enable) g_log_color = 0;
}

/* 串口带色输出 + 缓冲纯文本（调用方持有 g_log_lock） */
static void log_emit_colored(const char *color, const char *a,
                             const char *b, const char *c) {
    serial_write(color);
    serial_write(a);
    if (b) serial_write(b);
    if (c) serial_write(c);
    serial_write("\x1b[0m\n");
    if (g_log_capture_on) {
        const char *parts[3] = { a, b, c };
        for (int i = 0; i < 3; i++) {
            const char *s = parts[i];
            if (!s) continue;
            while (*s) {
                if (g_log_len < LOG_BUF_SIZE) {
                    g_log_buf[g_log_len++] = *s;
                }
                s++;
            }
        }
        if (g_log_len < LOG_BUF_SIZE) {
            g_log_buf[g_log_len++] = '\n';
        }
    }
}

static void log_line(const char *level, const char *msg) {
    /* 整行一把锁：避免 BSP/AP 的日志把一行拆成两半。 */
    utsm_spin_lock(&g_log_lock);
    if (g_log_color && g_log_color_on) {
        log_emit_colored(g_log_color, level, " ", msg);
    } else {
        log_emit_raw(level);
        log_emit_raw(" ");
        log_emit_raw(msg);
        log_emit_raw("\n");
    }
    utsm_spin_unlock(&g_log_lock);
}

/* 初始化画面的转圈挂在日志上：启动期几乎每一步都打日志。 */
extern void boot_status_pump(void);

void log_info(const char *msg) {
    log_line("[INFO]", msg);
    boot_status_pump();
}

void log_warn(const char *msg) {
    log_line("[WARN]", msg);
    boot_status_pump();
}

void log_error(const char *msg) {
    log_line("[ERR ]", msg);
    boot_status_pump();
}

void log_hex64(const char *prefix, u64 value) {
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xf];
    }
    buf[18] = 0;
    utsm_spin_lock(&g_log_lock);
    if (g_log_color && g_log_color_on) {
        log_emit_colored(g_log_color, prefix, buf, 0);
    } else {
        log_emit_raw(prefix);
        log_emit_raw(buf);
        log_emit_raw("\n");
    }
    utsm_spin_unlock(&g_log_lock);
    boot_status_pump();
}
