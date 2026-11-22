/* Deshab CMD — Windows 风格命令行（PE/EXE 兼容层前端）
 *
 * 黑底白字 cmd.exe 风格，PS/2 键盘输入。
 * 内置命令：dir/cd/type/copy/del/ren/mkdir/rmdir/echo/cls/ver/set/help/exit
 * PE 执行：输入 foo.exe 或 foo → 从 FAT32 根读取 → 调用 UTSM PE 服务执行
 *   - PE32+ (64位)：UTSM 原生 Ring0 执行
 *   - PE32  (32位)：UTSM 内嵌 x86-32 解释器执行
 * Esc 返回桌面。
 */

#include "../UTSM/include/utsm/dsk.h"
#include "../UTSM/include/utsm/pe.h"
#include "../UTSM/pe/pe_loader.h"   /* PE 头结构体（image_dos_header 等），供 peinfo 解析 */
#include "../DKM/dkm_shared.h"       /* P4f: DKM 驱动 ABI（driver_init/desc/kernel_api/bus） */
#include "../DKM/dkm_input.h"         /* P6b: srv.input.kbd.poll 客户端 */

/* P6b：输入经服务总线（srv.input.kbd.poll）。 */
static dkm_input g_input;
static int g_input_logged;

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#define COM1 0x3F8

static __inline__ void outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port)); }
static __inline__ u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

/* ---- TSC 计时 ---- */
static u64 g_tsc_per_ms = 0;

static __inline__ u64 rdtsc_cmd(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void tsc_calibrate_cmd(void) {
    outb(0x43, 0x30);
    outb(0x40, 0x7c);
    outb(0x40, 0x2e);
    u64 tsc_start = rdtsc_cmd();
    u16 prev = 0; u64 loops = 0;
    for (;;) {
        outb(0x43, 0x00);
        u16 cur = (u16)inb(0x40) | ((u16)inb(0x40) << 8);
        if (cur > prev && loops > 10) break;
        prev = cur; loops++;
    }
    u64 tsc_end = rdtsc_cmd();
    g_tsc_per_ms = (tsc_end - tsc_start) / 10;
}

static void serial_wait_tx(void) {
    if (g_tsc_per_ms == 0) tsc_calibrate_cmd();
    if (g_tsc_per_ms == 0) {
        for (u32 i = 0; i < 100000; i++) { if (inb(COM1 + 5) & 0x20) break; }
        return;
    }
    u64 deadline = rdtsc_cmd() + g_tsc_per_ms / 10;
    while (rdtsc_cmd() < deadline) {
        if (inb(COM1 + 5) & 0x20) break;
        __asm__ volatile("pause");
    }
}

static void sputc(char c) { serial_wait_tx(); outb(COM1, (unsigned char)c); }
static void swrite(const char *s) { while (*s) { if (*s == '\n') sputc('\r'); sputc(*s++); } }
static void logl(const char *s) { swrite(s); swrite("\n"); }

/* ASCII 字体 — 必须在 deshab_ui.h 之前包含 */
#include "../firstInit/ascii_bitmaps.c"
#include "../UTSM/include/utsm/deshab_ui.h"
/* FAT32 共享读写库 — PE64 程序（如 deaicup.exe ~2.2MB）需要大文件缓冲 */
#define F32_DATA_BYTES (4u * 1024u * 1024u)
#include "../tools/fat32_io.h"

/* ============================================================
 *  颜色定义 — Windows cmd.exe 风格（黑底浅灰字）
 * ============================================================ */

#define CMD_BG       0xFF000000u   /* 纯黑背景 */
#define CMD_TEXT     0xFFC0C0C0u   /* 浅灰文字（cmd.exe 默认） */
#define CMD_PROMPT   0xFFFFFFFFu   /* 白色提示符 */
#define CMD_ERROR    0xFFFF6060u   /* 浅红错误 */
#define CMD_OK       0xFF60FF60u   /* 浅绿成功 */
#define CMD_INFO     0xFF60A0FFu   /* 浅蓝信息 */
#define CMD_DIM      0xFF808080u   /* 灰色暗淡 */
#define CMD_WARN     0xFFFFFF60u   /* 黄色警告 */

/* ============================================================
 *  终端缓冲区与渲染
 * ============================================================ */

#define TERM_COLS  100
#define TERM_ROWS  40
#define TERM_MAX_CHARS (TERM_COLS * TERM_ROWS)

static du_context g_ctx;
static u64 fb_a, fb_w, fb_h, fb_p;

static u8  term_ch[TERM_MAX_CHARS];
static u32 term_fg[TERM_MAX_CHARS];
static int term_w = TERM_COLS;
static int term_h = TERM_ROWS;
static int cur_col = 0;
static int cur_row = 0;
static int cur_visible = 1;
static int term_start_x, term_start_y;

static const dsk_boot_context *g_boot_ctx = 0;
static u64 g_kernel_api = 0;
static const pe_service *g_pe_svc = 0;

/* ---- 实机多盘：fat32_io.h 硬编码块设备 0 ----
 * 与 desktop 同理：经 srv.fs.fat32.volume_info 取启动卷所在设备并重定向。 */
static f32_block_read_fn  g_f32_read_raw;
static f32_block_write_fn g_f32_write_raw;
static u32 g_f32_dev;

static int cmd_f32_read(u32 index, u64 lba, u32 count, void *buffer) {
    (void)index;
    return g_f32_read_raw ? g_f32_read_raw(g_f32_dev, lba, count, buffer) : -1;
}
static int cmd_f32_write(u32 index, u64 lba, u32 count, const void *buffer) {
    (void)index;
    return g_f32_write_raw ? g_f32_write_raw(g_f32_dev, lba, count, buffer) : -1;
}

/* 从总线解析启动卷设备索引；失败保持 0（单盘场景行为不变）。 */
static void cmd_f32_init_from_api(const struct dkm_kernel_api *api) {
    g_f32_read_raw  = (f32_block_read_fn)(api->block ? api->block->read  : 0);
    g_f32_write_raw = (f32_block_write_fn)(api->block ? api->block->write : 0);
    g_f32_dev = 0;
    {
        void *fn = 0;
        if (api->bus && api->bus->probe("srv.fs.fat32.volume_info", &fn, 0) == 0 && fn) {
            const struct dkm_fat32_volume_info *(*get)(void) =
                (const struct dkm_fat32_volume_info *(*)(void))fn;
            const struct dkm_fat32_volume_info *vi = get();
            if (vi && vi->valid) g_f32_dev = vi->dev;
        }
    }
    f32_init(g_f32_read_raw ? cmd_f32_read : 0,
             g_f32_write_raw ? cmd_f32_write : 0);
}

/* dev_mode 串口镜像 */
static int g_serial_mirror = 0;

static void term_init_pos(void) {
    int margin = 4;
    term_start_x = margin;
    term_start_y = margin;
    int avail_w = (int)fb_w - margin * 2;
    int avail_h = (int)fb_h - margin * 2;
    term_w = avail_w / (int)DU_ASCII_STEP;
    term_h = avail_h / (int)DU_ASCII_LINE_H;
    if (term_w < 10) term_w = 10;
    if (term_h < 5) term_h = 5;
    if (term_w > TERM_COLS) term_w = TERM_COLS;
    if (term_h > TERM_ROWS) term_h = TERM_ROWS;
}

static void term_clear_buf(void) {
    for (int i = 0; i < TERM_MAX_CHARS; i++) {
        term_ch[i] = ' ';
        term_fg[i] = CMD_TEXT;
    }
    cur_col = 0;
    cur_row = 0;
}

static void term_scroll(void) {
    for (int r = 1; r < term_h; r++) {
        for (int c = 0; c < term_w; c++) {
            int src = r * TERM_COLS + c;
            int dst = (r - 1) * TERM_COLS + c;
            term_ch[dst] = term_ch[src];
            term_fg[dst] = term_fg[src];
        }
    }
    for (int c = 0; c < term_w; c++) {
        term_ch[(term_h - 1) * TERM_COLS + c] = ' ';
        term_fg[(term_h - 1) * TERM_COLS + c] = CMD_TEXT;
    }
    cur_row = term_h - 1;
    cur_col = 0;
}

static void term_newline(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= term_h) term_scroll();
}

static void term_putc_color(char c, u32 color) {
    if (g_serial_mirror) { if (c == '\n') sputc('\r'); sputc(c); }
    if (c == '\n') { term_newline(); return; }
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\t') {
        int spaces = 8 - (cur_col % 8);
        for (int i = 0; i < spaces; i++) term_putc_color(' ', color);
        return;
    }
    if (c < ' ' || c > '~') c = '.';
    if (cur_col >= term_w) term_newline();
    int idx = cur_row * TERM_COLS + cur_col;
    term_ch[idx] = (u8)c;
    term_fg[idx] = color;
    cur_col++;
}

static void term_putc(char c) { term_putc_color(c, CMD_TEXT); }
static void term_puts_color(const char *s, u32 color) { while (*s) term_putc_color(*s++, color); }
static void term_puts(const char *s) { term_puts_color(s, CMD_TEXT); }

/* ---- 数字转字符串 ---- */
static void term_putu(u32 v) {
    char buf[12];
    int i = 11;
    buf[i--] = 0;
    if (v == 0) { buf[i--] = '0'; }
    while (v > 0 && i >= 0) { buf[i--] = (char)('0' + (v % 10)); v /= 10; }
    term_puts(&buf[i + 1]);
}

static void term_putx(u32 v) {
    static const char hex[] = "0123456789ABCDEF";
    term_putc('0'); term_putc('x');
    for (int i = 28; i >= 0; i -= 4) {
        u32 nib = (v >> i) & 0xF;
        if (nib != 0 || i <= 12) term_putc(hex[nib]);
    }
}

/* 64 位十六进制输出（前导零抑制），用于 PE ImageBase 等宽字段 */
static void term_putx64(u64 v) {
    static const char hex[] = "0123456789ABCDEF";
    term_putc('0'); term_putc('x');
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        u64 nib = (v >> i) & 0xF;
        if (nib) started = 1;
        if (started || i == 0) term_putc(hex[nib]);
    }
}

/* ---- 终端全屏渲染 ---- */
static void term_redraw_all(void) {
    du_fill_rect(&g_ctx, 0, 0, (i64)fb_w, (i64)fb_h, CMD_BG);
    for (int r = 0; r < term_h; r++) {
        i64 y = term_start_y + (i64)r * (i64)DU_ASCII_LINE_H;
        for (int c = 0; c < term_w; c++) {
            int idx = r * TERM_COLS + c;
            i64 x = term_start_x + (i64)c * (i64)DU_ASCII_STEP;
            du_draw_char(&g_ctx, (char)term_ch[idx], x, y, term_fg[idx], CMD_BG);
        }
    }
}

static void term_redraw_cursor(void) {
    if (cur_col >= term_w) return;
    i64 x = term_start_x + (i64)cur_col * (i64)DU_ASCII_STEP;
    i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
    du_fill_rect(&g_ctx, x, y, (i64)DU_ASCII_STEP - 1, (i64)DU_ASCII_CELL_H, CMD_PROMPT);
}

static void term_clear_cursor(void) {
    if (cur_col >= term_w) return;
    i64 x = term_start_x + (i64)cur_col * (i64)DU_ASCII_STEP;
    i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
    int idx = cur_row * TERM_COLS + cur_col;
    du_fill_rect(&g_ctx, x, y, (i64)DU_ASCII_STEP - 1, (i64)DU_ASCII_CELL_H, CMD_BG);
    du_draw_char(&g_ctx, (char)term_ch[idx], x, y, term_fg[idx], CMD_BG);
}

/* ============================================================
 *  键盘输入（PS/2 Scan Code Set 1）
 * ============================================================ */

static char scan_to_ascii(u8 sc, int shift) {
    static const char normal[58] = {
        0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, '\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'', '`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    static const char shifted[58] = {
        0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, '\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };
    if (sc >= 58) return 0;
    return shift ? shifted[sc] : normal[sc];
}

/* ============================================================
 *  字符串工具
 * ============================================================ */

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ============================================================
 *  输入行编辑
 * ============================================================ */

static char input_buf[256];
static int  input_len = 0;
static int  input_cursor = 0;

static const char *PROMPT = "C:\\> ";

static void redraw_input_line(void) {
    int prompt_len = str_len(PROMPT);
    i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
    du_fill_rect(&g_ctx, term_start_x, y,
                 (i64)term_w * (i64)DU_ASCII_STEP,
                 (i64)DU_ASCII_CELL_H, CMD_BG);
    for (int i = 0; i < prompt_len; i++) {
        i64 x = term_start_x + (i64)i * (i64)DU_ASCII_STEP;
        du_draw_char(&g_ctx, PROMPT[i], x, y, CMD_PROMPT, CMD_BG);
    }
    for (int i = 0; i < input_len; i++) {
        i64 x = term_start_x + (i64)(prompt_len + i) * (i64)DU_ASCII_STEP;
        du_draw_char(&g_ctx, input_buf[i], x, y, CMD_TEXT, CMD_BG);
    }
    cur_col = prompt_len + input_cursor;
    term_redraw_cursor();
}

static void insert_char(char c) {
    if (input_len >= 255) return;
    for (int i = input_len; i > input_cursor; i--)
        input_buf[i] = input_buf[i - 1];
    input_buf[input_cursor] = c;
    input_len++;
    input_cursor++;
    redraw_input_line();
}

static void delete_char_back(void) {
    if (input_cursor == 0) return;
    for (int i = input_cursor - 1; i < input_len - 1; i++)
        input_buf[i] = input_buf[i + 1];
    input_len--;
    input_cursor--;
    input_buf[input_len] = 0;
    redraw_input_line();
}

static void cursor_left(void) {
    if (input_cursor > 0) { input_cursor--; redraw_input_line(); }
}

static void cursor_right(void) {
    if (input_cursor < input_len) { input_cursor++; redraw_input_line(); }
}

static void cursor_home(void) {
    input_cursor = 0;
    redraw_input_line();
}

static void cursor_end(void) {
    input_cursor = input_len;
    redraw_input_line();
}

static void kill_line(void) {
    input_len = 0;
    input_cursor = 0;
    input_buf[0] = 0;
    redraw_input_line();
}

/* ============================================================
 *  内置命令
 * ============================================================ */

static void cmd_help(void) {
    term_puts_color("Deshab CMD — 命令列表:\r\n", CMD_INFO);
    term_puts("  dir          列出文件\r\n");
    term_puts("  cd           显示当前目录\r\n");
    term_puts("  type <file>  显示文件内容\r\n");
    term_puts("  copy <a> <b> 复制文件\r\n");
    term_puts("  del <file>   删除文件\r\n");
    term_puts("  ren <a> <b>  重命名文件\r\n");
    term_puts("  echo <text>  显示文本 (echo text > file 写入)\r\n");
    term_puts("  cls          清屏\r\n");
    term_puts("  ver          显示版本\r\n");
    term_puts("  set          显示环境变量\r\n");
    term_puts("  help         显示此帮助\r\n");
    term_puts("  shutdown     关机 (ACPI S5; 别名 poweroff/halt)\r\n");
    term_puts("  reboot       重启 (ACPI reset)\r\n");
    term_puts("  exit         返回桌面\r\n");
    term_puts_color("\nPE 程序执行:\r\n", CMD_WARN);
    term_puts("  <name>.exe   运行 PE32+/PE32 程序 (如 hello.exe)\r\n");
    term_puts("  <name>       自动查找 name.exe 并运行\r\n");
    term_puts("  pe <name>    显式运行 PE 程序 (如 pe hello64)\r\n");
    term_puts("  peinfo <nm>  显示 PE 头信息 (不执行)\r\n");
}

static void cmd_ver(void) {
    term_puts_color("Deshab CMD [Version 0.1.0]\r\n", CMD_OK);
    term_puts_color("(c) 2026 Deshab Project. PE/EXE 兼容层 by UTSM.\r\n", CMD_DIM);
}

static void cmd_cls(void) {
    term_clear_buf();
    term_redraw_all();
}

static void cmd_cd(void) {
    term_puts("C:\\\r\n");
}

static void cmd_set(void) {
    term_puts_color("环境变量:\r\n", CMD_INFO);
    term_puts("  PATH=C:\\;C:\\bin\r\n");
    term_puts("  PROMPT=$P$G\r\n");
    term_puts("  OS=Deshab\r\n");
    term_puts("  PE_COMPAT=UTSM_PE32+_PE32\r\n");
}

/* dir — 列出 FAT32 根目录文件 */
typedef struct {
    int count;
    u32 total_bytes;
} dir_ctx_static;

static int dir_list_cb(const char *name, u32 size, u8 attr, void *ud) {
    dir_ctx_static *st = (dir_ctx_static *)ud;
    if (attr & 0x10) {
        term_puts_color(name, CMD_INFO);
        term_puts_color("  <DIR>        ", CMD_DIM);
    } else {
        term_puts(name);
        term_puts("  ");
        /* 右对齐文件大小 */
        u32 sz = size;
        char numbuf[12];
        int ni = 11;
        numbuf[ni--] = 0;
        if (sz == 0) numbuf[ni--] = '0';
        while (sz > 0 && ni >= 0) { numbuf[ni--] = (char)('0' + sz % 10); sz /= 10; }
        while (ni >= 0) numbuf[ni--] = ' ';
        term_puts(&numbuf[0]);
        st->total_bytes += size;
    }
    term_puts("\r\n");
    st->count++;
    return 0;
}

static void cmd_dir(void) {
    if (!f32_blk_read) {
        term_puts_color("dir: 无块设备\r\n", CMD_ERROR);
        return;
    }
    term_puts_color(" C 盘的卷是 DESHAB\r\n", CMD_DIM);
    term_puts_color(" 卷序列号为 DE5A-B002\r\n", CMD_DIM);
    term_puts("\r\n C:\\ 的目录\r\n\r\n");

    dir_ctx_static st = {0, 0};
    f32_list_root(dir_list_cb, &st);

    term_puts("\r\n");
    term_putu((u32)st.count);
    term_puts(" 个文件  ");
    term_putu(st.total_bytes);
    term_puts(" 字节\r\n");
}

/* type <file> — 显示文件内容 */
static void cmd_type(const char *args) {
    if (!*args) { term_puts_color("type: 缺少文件名\r\n", CMD_ERROR); return; }
    if (!f32_blk_read) { term_puts_color("type: 无块设备\r\n", CMD_ERROR); return; }

    char name11[11];
    if (f32_name_to_83(args, name11) != 0) {
        term_puts_color("type: 无效文件名\r\n", CMD_ERROR);
        return;
    }
    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file(name11, &data, &size) != 0) {
        term_puts_color("type: 文件不存在\r\n", CMD_ERROR);
        return;
    }
    /* 输出文件内容（按字符） */
    for (u32 i = 0; i < size; i++) {
        char c = (char)data[i];
        if (c == '\n') term_putc('\r');
        term_putc(c);
    }
    if (size > 0 && data[size - 1] != '\n') term_putc('\r');
    term_putc('\n');
}

/* echo <text> — 显示文本或重定向到文件 */
static void cmd_echo(const char *args) {
    /* 检查重定向: echo text > file */
    char text[256];
    str_copy(text, args, sizeof(text));

    /* 查找 > */
    int redir = -1;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '>') { redir = i; break; }
    }

    if (redir >= 0) {
        /* 重定向到文件 */
        text[redir] = 0;
        /* 去尾部空格 */
        int tl = str_len(text);
        while (tl > 0 && text[tl - 1] == ' ') text[--tl] = 0;

        /* 获取文件名 */
        char *fn = text + redir + 1;
        while (*fn == ' ') fn++;
        if (!*fn) { term_puts_color("echo: 重定向缺少文件名\r\n", CMD_ERROR); return; }

        char name11[11];
        if (f32_name_to_83(fn, name11) != 0) {
            term_puts_color("echo: 无效文件名\r\n", CMD_ERROR);
            return;
        }
        if (!f32_blk_write) { term_puts_color("echo: 无块设备写能力\r\n", CMD_ERROR); return; }

        /* 构造写入内容 */
        char content[256];
        int ci = 0;
        for (int i = 0; i < tl && ci < 254; i++) content[ci++] = text[i];
        content[ci++] = '\r';
        content[ci++] = '\n';

        if (f32_write_root_file(name11, (const u8 *)content, (u32)ci) != 0) {
            term_puts_color("echo: 写入失败\r\n", CMD_ERROR);
            return;
        }
        term_puts_color("echo: 已写入 ", CMD_OK);
        term_puts(fn);
        term_puts("\r\n");
    } else {
        /* 直接显示 */
        if (*args) {
            term_puts(args);
        }
        term_puts("\r\n");
    }
}

/* copy <src> <dst> — 复制文件 */
static void cmd_copy(const char *args) {
    char src[64], dst[64];
    int si = 0, di = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && si < 63) src[si++] = *p++;
    src[si] = 0;
    while (*p == ' ') p++;
    while (*p && di < 63) dst[di++] = *p++;
    dst[di] = 0;

    if (!src[0] || !dst[0]) {
        term_puts_color("copy: 用法 copy <源文件> <目标文件>\r\n", CMD_ERROR);
        return;
    }
    if (!f32_blk_read || !f32_blk_write) {
        term_puts_color("copy: 无块设备\r\n", CMD_ERROR);
        return;
    }

    char s83[11], d83[11];
    if (f32_name_to_83(src, s83) != 0 || f32_name_to_83(dst, d83) != 0) {
        term_puts_color("copy: 无效文件名\r\n", CMD_ERROR);
        return;
    }

    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file(s83, &data, &size) != 0) {
        term_puts_color("copy: 源文件不存在\r\n", CMD_ERROR);
        return;
    }
    if (f32_write_root_file(d83, data, size) != 0) {
        term_puts_color("copy: 写入失败\r\n", CMD_ERROR);
        return;
    }
    term_puts_color("        1 个文件已复制。\r\n", CMD_OK);
}

/* del <file> — 删除文件 */
static void cmd_del(const char *args) {
    if (!*args) { term_puts_color("del: 缺少文件名\r\n", CMD_ERROR); return; }
    if (!f32_blk_write) { term_puts_color("del: 无块设备写能力\r\n", CMD_ERROR); return; }

    char name11[11];
    if (f32_name_to_83(args, name11) != 0) {
        term_puts_color("del: 无效文件名\r\n", CMD_ERROR);
        return;
    }
    if (f32_delete_root_file(name11) != 0) {
        term_puts_color("del: 文件不存在或删除失败\r\n", CMD_ERROR);
        return;
    }
    term_puts_color("del: 已删除\r\n", CMD_OK);
}

/* ren <old> <new> — 重命名文件 */
static void cmd_ren(const char *args) {
    char old[64], nw[64];
    int oi = 0, ni = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && oi < 63) old[oi++] = *p++;
    old[oi] = 0;
    while (*p == ' ') p++;
    while (*p && ni < 63) nw[ni++] = *p++;
    nw[ni] = 0;

    if (!old[0] || !nw[0]) {
        term_puts_color("ren: 用法 ren <旧名> <新名>\r\n", CMD_ERROR);
        return;
    }
    if (!f32_blk_read || !f32_blk_write) {
        term_puts_color("ren: 无块设备\r\n", CMD_ERROR);
        return;
    }

    char o83[11], n83[11];
    if (f32_name_to_83(old, o83) != 0 || f32_name_to_83(nw, n83) != 0) {
        term_puts_color("ren: 无效文件名\r\n", CMD_ERROR);
        return;
    }

    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file(o83, &data, &size) != 0) {
        term_puts_color("ren: 源文件不存在\r\n", CMD_ERROR);
        return;
    }
    if (f32_write_root_file(n83, data, size) != 0) {
        term_puts_color("ren: 写入新文件失败\r\n", CMD_ERROR);
        return;
    }
    if (f32_delete_root_file(o83) != 0) {
        term_puts_color("ren: 删除旧文件失败（警告）\r\n", CMD_WARN);
    }
    term_puts_color("ren: 已重命名\r\n", CMD_OK);
}

static void cmd_mkdir(void) { term_puts_color("mkdir: FAT32 根目录不支持子目录\r\n", CMD_WARN); }
static void cmd_rmdir(void) { term_puts_color("rmdir: FAT32 根目录不支持子目录\r\n", CMD_WARN); }

/* ============================================================
 *  PE/EXE 程序执行
 * ============================================================ */

/* 检查文件名是否以 .exe/.com/.bat 结尾（不区分大小写） */
static int has_exec_ext(const char *name) {
    int len = str_len(name);
    if (len < 4) return 0;
    const char *ext = name + len - 4;
    if (ext[0] != '.') return 0;
    if (str_ieq(ext, ".exe")) return 1;
    if (str_ieq(ext, ".com")) return 1;
    if (str_ieq(ext, ".bat")) return 2;
    return 0;
}

/* 执行 PE/EXE 程序。
 * prog_name = 程序名（可能带或不带 .exe 后缀）
 * full_cmdline = 完整命令行（程序名 + 参数，即 GetCommandLineA 返回值）
 */
static int cmd_read_pe_from_bin(const char *name11, u8 **data, u32 *size);  /* 前向声明 */
static int cmd_read_pe_from_program(const char *name11, u8 **data, u32 *size);

static void exec_pe(const char *prog_name, const char *full_cmdline) {
    if (!g_pe_svc) {
        term_puts_color("PE 兼容层不可用（UTSM PE 服务未初始化）\r\n", CMD_ERROR);
        return;
    }
    if (!f32_blk_read) {
        term_puts_color("无块设备，无法读取 PE 文件\r\n", CMD_ERROR);
        return;
    }

    /* 构造 8.3 文件名 */
    char base[64];
    str_copy(base, prog_name, sizeof(base));

    /* 如果没有扩展名，自动补 .exe */
    int has_dot = 0;
    for (int i = 0; base[i]; i++) {
        if (base[i] == '.') { has_dot = 1; break; }
    }
    if (!has_dot) {
        int bl = str_len(base);
        if (bl > 59) bl = 59;
        base[bl] = '.'; base[bl + 1] = 'e'; base[bl + 2] = 'x';
        base[bl + 3] = 'e'; base[bl + 4] = 0;
    }

    char name11[11];
    if (f32_name_to_83(base, name11) != 0) {
        term_puts_color("无效文件名\r\n", CMD_ERROR);
        return;
    }

    /* PATH 查找：根目录 → /bin/ → /program/（约定：bin=elf 工具，program=exe） */
    u8 *pe_data = 0; u32 pe_size = 0;
    int rc = f32_read_root_file(name11, &pe_data, &pe_size);
    if (rc != 0) {
        rc = cmd_read_pe_from_bin(name11, &pe_data, &pe_size);
    }
    if (rc != 0) {
        rc = cmd_read_pe_from_program(name11, &pe_data, &pe_size);
    }
    if (rc != 0) {
        term_puts_color("'", CMD_ERROR);
        term_puts_color(prog_name, CMD_ERROR);
        term_puts_color("' 不是内部或外部命令，也不是可运行的程序。\r\n", CMD_ERROR);
        term_puts_color("  (已搜索 C:\\ 、C:\\bin\\ 及 C:\\program\\)\r\n", CMD_DIM);
        return;
    }

    /* 验证 PE 头（MZ 签名） */
    if (pe_size < 2 || pe_data[0] != 'M' || pe_data[1] != 'Z') {
        term_puts_color("不是有效的 PE/EXE 文件（缺少 MZ 签名）\r\n", CMD_ERROR);
        return;
    }

    term_puts_color("正在执行 ", CMD_INFO);
    term_puts_color(prog_name, CMD_WARN);
    term_puts_color(" ...\r\n", CMD_INFO);
    term_redraw_all();

    /* 调用 UTSM PE 服务执行 */
    logl("[cmd] PE run begin");
    swrite("[cmd] cmdline: "); swrite(full_cmdline); swrite("\n");

    u64 exit_code = 0;
    rc = g_pe_svc->run(pe_data, (u64)pe_size, full_cmdline, &exit_code);

    if (rc != 0) {
        term_puts_color("PE 执行失败，错误码: ", CMD_ERROR);
        term_putx((u32)rc);
        term_puts("\r\n");
    } else {
        term_puts_color("程序退出，返回码: ", CMD_DIM);
        term_putu((u32)exit_code);
        term_puts("\r\n");
    }

    logl("[cmd] PE run end");
}

/* ============================================================
 *  /bin 子目录支持 — 用于查找 PE 可执行程序
 * ============================================================ */

/* 在根目录簇链中查找名为 name11 的子目录（attr & 0x10），返回首簇号。
 * 使用 f32_disk 缓存（来自 fat32_io.h）。返回 0 成功，-1 未找到。 */
static int cmd_find_subdir(u32 dir_clus, const char *name11, u32 *out_clus) {
    if (!f32_disk_loaded && f32_disk_load() != 0) return -2;
    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;
    u32 clus = dir_clus;
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        u32 lba = data_lba + (clus - 2) * spc;
        const u8 *cb;
        if ((u64)lba * 512 + (u64)spc * 512 <= sizeof(f32_disk))
            cb = f32_disk + (u64)lba * 512;
        else {
            if (f32_read_sectors(lba, 8, f32_cluster) != 0) return -4;
            cb = f32_cluster;
        }
        const f32_dirent *dir = (const f32_dirent *)cb;
        for (u32 e = 0; e * 32 < spc * 512; e++) {
            if (dir[e].name[0] == 0) goto done_dir;
            if ((u8)dir[e].name[0] == 0xE5) continue;
            if (dir[e].attr == 0x0F) continue;
            if (dir[e].attr & 0x08) continue;
            if (!(dir[e].attr & 0x10)) continue;  /* 只匹配目录 */
            if (f32_neq11(dir[e].name, name11)) {
                *out_clus = f32_r16((const u8*)&dir[e].clow) | ((u32)f32_r16((const u8*)&dir[e].chigh) << 16);
                return 0;
            }
        }
        u32 fo = fat_byte_off + clus * 4;
        if (fo + 4 > sizeof(f32_disk)) break;
        clus = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
    }
done_dir:
    return -1;
}

/* 从指定目录首簇读取文件到 f32_data。
 * dir_clus: 目录起始簇号。name11: 8.3 名。
 * 返回 0 成功，*out_data 指向 f32_data，*out_size 为字节数；非 0 失败。 */
static int cmd_read_file_in_dir(u32 dir_clus, const char *name11, u8 **out_data, u32 *out_size) {
    if (!f32_disk_loaded && f32_disk_load() != 0) return -1;
    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;

    /* 先在目录中查找文件 */
    u32 clus = dir_clus;
    u32 found_clus = 0, found_size = 0;
    int found = 0;
    while (clus >= 2 && clus < 0x0FFFFFF8 && !found) {
        u32 lba = data_lba + (clus - 2) * spc;
        const u8 *cb;
        if ((u64)lba * 512 + (u64)spc * 512 <= sizeof(f32_disk))
            cb = f32_disk + (u64)lba * 512;
        else {
            if (f32_read_sectors(lba, 8, f32_cluster) != 0) return -4;
            cb = f32_cluster;
        }
        const f32_dirent *dir = (const f32_dirent *)cb;
        for (u32 e = 0; e * 32 < spc * 512; e++) {
            if (dir[e].name[0] == 0) goto done_find;
            if ((u8)dir[e].name[0] == 0xE5) continue;
            if (dir[e].attr == 0x0F) continue;
            if (dir[e].attr & 0x08) continue;
            if (dir[e].attr & 0x10) continue;  /* 跳过目录 */
            if (f32_neq11(dir[e].name, name11)) {
                found_clus = f32_r16((const u8*)&dir[e].clow) | ((u32)f32_r16((const u8*)&dir[e].chigh) << 16);
                found_size = dir[e].fsize;
                found = 1;
                break;
            }
        }
        u32 fo = fat_byte_off + clus * 4;
        if (fo + 4 > sizeof(f32_disk)) break;
        clus = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
    }
done_find:
    if (!found) return -5;
    if (found_size > sizeof(f32_data)) return -6;

    /* 读取文件数据 */
    u8 *dst = f32_data; u32 remaining = found_size; u32 fc = found_clus;
    while (fc >= 2 && fc < 0x0FFFFFF8 && remaining > 0) {
        u32 fc_lba = data_lba + (fc - 2) * spc;
        u32 fc_bytes = spc * 512;
        if (fc_bytes > remaining) fc_bytes = remaining;
        const u8 *fb;
        if ((u64)fc_lba * 512 + (u64)spc * 512 <= sizeof(f32_disk))
            fb = f32_disk + (u64)fc_lba * 512;
        else {
            if (f32_read_sectors(fc_lba, 8, f32_cluster) != 0) return -7;
            fb = f32_cluster;
        }
        for (u32 b = 0; b < fc_bytes; b++) dst[b] = fb[b];
        dst += fc_bytes; remaining -= fc_bytes;
        u32 fo = fat_byte_off + fc * 4;
        if (fo + 4 > sizeof(f32_disk)) break;
        fc = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
    }
    *out_data = f32_data; *out_size = found_size;
    return 0;
}

static u32 g_cmd_bin_clus = 0;
static int g_cmd_bin_checked = 0;

/* 获取 /bin 目录首簇号。返回 0 成功，-1 未找到。 */
static int cmd_get_bin_cluster(u32 *out_clus) {
    if (g_cmd_bin_checked) {
        if (g_cmd_bin_clus == 0) return -1;
        *out_clus = g_cmd_bin_clus;
        return 0;
    }
    g_cmd_bin_checked = 1;
    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    char bin83[11] = {'B','I','N',' ',' ',' ',' ',' ',' ',' ',' '};
    if (cmd_find_subdir(bpb->root_clus, bin83, &g_cmd_bin_clus) != 0) {
        g_cmd_bin_clus = 0;
        return -1;
    }
    *out_clus = g_cmd_bin_clus;
    return 0;
}

/* 从 /bin 子目录读取 PE 文件。返回 0 成功。 */
static int cmd_read_pe_from_bin(const char *name11, u8 **data, u32 *size) {
    u32 bin_clus;
    if (cmd_get_bin_cluster(&bin_clus) != 0) return -1;
    return cmd_read_file_in_dir(bin_clus, name11, data, size);
}

/* 从 /program 子目录读取 PE 文件（exe 约定目录：bin=elf 工具，program=exe）。 */
static int cmd_read_pe_from_program(const char *name11, u8 **data, u32 *size) {
    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    char prog83[11] = {'P','R','O','G','R','A','M',' ',' ',' ',' '};
    u32 clus;
    if (cmd_find_subdir(bpb->root_clus, prog83, &clus) != 0) return -1;
    return cmd_read_file_in_dir(clus, name11, data, size);
}

/* ============================================================
 *  peinfo — 解析并显示 PE 头信息（不执行）
 * ============================================================ */

/* 把程序名转为 8.3 名（自动补 .exe），返回 0 成功，-1 失败。
 * 内联自 exec_pe 的同名逻辑，供 cmd_peinfo 复用。 */
static int make_pe_name83(const char *prog, char name11[11]) {
    char base[64];
    str_copy(base, prog, sizeof(base));
    int has_dot = 0;
    for (int i = 0; base[i]; i++) {
        if (base[i] == '.') { has_dot = 1; break; }
    }
    if (!has_dot) {
        int bl = str_len(base);
        if (bl > 59) bl = 59;
        base[bl] = '.'; base[bl + 1] = 'e'; base[bl + 2] = 'x';
        base[bl + 3] = 'e'; base[bl + 4] = 0;
    }
    return f32_name_to_83(base, name11);
}

static void cmd_peinfo(const char *args) {
    if (!*args) {
        term_puts_color("用法: peinfo <程序名>  (如 peinfo hello64)\r\n", CMD_INFO);
        return;
    }
    if (!f32_blk_read) {
        term_puts_color("peinfo: 无块设备\r\n", CMD_ERROR);
        return;
    }

    char name11[11];
    if (make_pe_name83(args, name11) != 0) {
        term_puts_color("peinfo: 无效文件名\r\n", CMD_ERROR);
        return;
    }

    /* PATH 查找：先根目录，再 /bin/ */
    u8 *pe = 0; u32 pe_size = 0;
    int rc = f32_read_root_file(name11, &pe, &pe_size);
    if (rc != 0) {
        rc = cmd_read_pe_from_bin(name11, &pe, &pe_size);
    }
    if (rc != 0) {
        term_puts_color("peinfo: 文件不存在: ", CMD_ERROR);
        term_puts_color(args, CMD_ERROR);
        term_puts_color(" (已搜索 C:\\ 及 C:\\bin\\)\r\n", CMD_ERROR);
        return;
    }

    /* ---- DOS 头 ---- */
    if (pe_size < sizeof(image_dos_header)) {
        term_puts_color("peinfo: 文件过小，无 DOS 头\r\n", CMD_ERROR);
        return;
    }
    const image_dos_header *dos = (const image_dos_header *)pe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        term_puts_color("peinfo: 缺少 MZ 签名\r\n", CMD_ERROR);
        return;
    }
    u32 lfanew = dos->e_lfanew;
    if ((u64)lfanew + 4 + sizeof(image_file_header) > pe_size) {
        term_puts_color("peinfo: e_lfanew 越界\r\n", CMD_ERROR);
        return;
    }

    term_puts_color("=== PE 头信息 ===\r\n", CMD_INFO);
    term_puts("  DOS: MZ OK, e_lfanew=");
    term_putx(lfanew);
    term_puts("\r\n");

    /* ---- NT 签名 + File Header ---- */
    const u8 *nt = pe + lfanew;
    if (*(const u32 *)nt != IMAGE_NT_SIGNATURE) {
        term_puts_color("peinfo: 缺少 PE\\0\\0 签名\r\n", CMD_ERROR);
        return;
    }
    const image_file_header *fh = (const image_file_header *)(nt + 4);
    u16 machine = fh->machine;
    u16 nsec = fh->number_of_sections;
    u16 opt_size = fh->size_of_optional_header;

    term_puts("  NT: PE\\0\\0 OK\r\n");
    term_puts("  Machine: ");
    term_putx(machine);
    if (machine == 0x8664)      term_puts_color(" (AMD64)\r\n", CMD_OK);
    else if (machine == 0x014c) term_puts_color(" (i386)\r\n",  CMD_OK);
    else                        term_puts(" (未知)\r\n");
    term_puts("  节区数: ");
    term_putu(nsec);
    term_puts("  OptHdr 大小: ");
    term_putu(opt_size);
    term_puts("\r\n");

    /* ---- Optional Header ---- */
    if (opt_size < 2) {
        term_puts_color("peinfo: Optional Header 过小\r\n", CMD_WARN);
        return;
    }
    if ((u64)lfanew + 4 + sizeof(image_file_header) + opt_size > pe_size) {
        term_puts_color("peinfo: Optional Header 越界\r\n", CMD_ERROR);
        return;
    }
    const void *opt = (const void *)(nt + 4 + sizeof(image_file_header));
    u16 magic = *(const u16 *)opt;
    int is_pe32_plus = (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);

    u64 img_base = 0;
    u32 entry_rva = 0;
    u32 img_size = 0;
    u32 subsys = 0;
    const image_data_directory *import_dir = 0;

    if (is_pe32_plus) {
        if (opt_size >= sizeof(image_optional_header64)) {
            const image_optional_header64 *oh = (const image_optional_header64 *)opt;
            img_base = oh->image_base;
            entry_rva = oh->address_of_entry_point;
            img_size = oh->size_of_image;
            subsys = oh->subsystem;
            import_dir = &oh->data_directory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        }
        term_puts_color("  格式: PE32+ (64位 原生)\r\n", CMD_OK);
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        if (opt_size >= sizeof(image_optional_header32)) {
            const image_optional_header32 *oh = (const image_optional_header32 *)opt;
            img_base = oh->image_base;
            entry_rva = oh->address_of_entry_point;
            img_size = oh->size_of_image;
            subsys = oh->subsystem;
            import_dir = &oh->data_directory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        }
        term_puts_color("  格式: PE32 (32位 解释器)\r\n", CMD_OK);
    } else {
        term_puts_color("peinfo: 未知 Optional Header magic=", CMD_ERROR);
        term_putx(magic);
        term_puts("\r\n");
        return;
    }

    term_puts("  ImageBase:    ");
    term_putx64(img_base);
    term_puts("\r\n");
    term_puts("  EntryPoint:   ");
    term_putx(entry_rva);
    term_puts(" (RVA)\r\n");
    term_puts("  SizeOfImage:  ");
    term_putx(img_size);
    term_puts(" (");
    term_putu(img_size);
    term_puts(" bytes)\r\n");
    term_puts("  Subsystem:    ");
    term_putu(subsys);
    term_puts(subsys == 3 ? " (CUI 控制台)\r\n" : "\r\n");

    /* ---- 节区表 ---- */
    const image_section_header *secs = (const image_section_header *)
        ((const u8 *)opt + opt_size);
    if ((u64)((const u8 *)secs - pe) + (u64)nsec * sizeof(image_section_header) > pe_size) {
        term_puts_color("peinfo: 节区表越界，跳过\r\n", CMD_WARN);
        nsec = 0;
    }
    if (nsec > 0) {
        term_puts_color("  节区表:\r\n", CMD_INFO);
        term_puts("    Name       VAddr       VSize       RawSize     Flg\r\n");
        for (int i = 0; i < nsec && i < 96; i++) {
            char nm[9];
            for (int j = 0; j < 8; j++) nm[j] = (char)secs[i].name[j];
            nm[8] = 0;
            term_puts("    ");
            term_puts(nm);
            int pad = 8 - str_len(nm);
            for (int k = 0; k < pad; k++) term_putc(' ');
            term_puts("  ");
            term_putx(secs[i].virtual_address);
            term_puts("  ");
            term_putx(secs[i].virtual_size);
            term_puts("  ");
            term_putx(secs[i].size_of_raw_data);
            term_puts("  ");
            char flags[4];
            flags[0] = (secs[i].characteristics & IMAGE_SCN_MEM_EXECUTE) ? 'X' : '-';
            flags[1] = (secs[i].characteristics & IMAGE_SCN_MEM_READ)    ? 'R' : '-';
            flags[2] = (secs[i].characteristics & IMAGE_SCN_MEM_WRITE)   ? 'W' : '-';
            flags[3] = 0;
            term_puts_color(flags, CMD_WARN);
            term_puts("\r\n");
        }
    }

    /* ---- 导入表 ---- */
    if (import_dir && import_dir->virtual_address != 0 && import_dir->size != 0) {
        u32 imp_rva = import_dir->virtual_address;
        if (imp_rva >= pe_size) {
            term_puts_color("peinfo: 导入表 RVA 越界\r\n", CMD_WARN);
        } else {
            term_puts_color("  导入表:\r\n", CMD_INFO);
            int dll_count = 0;
            int func_total = 0;
            int truncated = 0;
            for (int di = 0; !truncated; di++) {
                u32 desc_off = imp_rva + (u32)di * sizeof(image_import_descriptor);
                if ((u64)desc_off + sizeof(image_import_descriptor) > pe_size) break;
                const image_import_descriptor *desc =
                    (const image_import_descriptor *)(pe + desc_off);
                if (desc->name == 0 && desc->first_thunk == 0) break;

                const char *dll_name = 0;
                if (desc->name < pe_size) dll_name = (const char *)(pe + desc->name);
                term_puts("    DLL: ");
                term_puts_color(dll_name ? dll_name : "(?)", CMD_WARN);
                term_puts("\r\n");

                u32 int_rva = desc->original_first_thunk;
                u32 iat_rva = desc->first_thunk;
                if (int_rva == 0) int_rva = iat_rva;
                if (int_rva == 0) { dll_count++; continue; }

                for (int fi = 0; ; fi++) {
                    u32 thunk_off = int_rva + (u32)fi * 4;
                    if ((u64)thunk_off + 4 > pe_size) break;
                    u32 thunk = *(const u32 *)(pe + thunk_off);
                    if (thunk == 0) break;

                    term_puts("      ");
                    if (thunk & 0x80000000u) {
                        term_puts("#ordinal ");
                        term_putu(thunk & 0xFFFF);
                    } else if (thunk + 2 < pe_size) {
                        const char *fn = (const char *)(pe + thunk + 2);
                        term_puts(fn);
                    } else {
                        term_puts("(?)");
                    }
                    term_puts("\r\n");
                    func_total++;
                    if (func_total > 64) {
                        term_puts_color("      ... (导入过多，截断)\r\n", CMD_DIM);
                        truncated = 1;
                        break;
                    }
                }
                dll_count++;
                if (dll_count > 32) {
                    term_puts_color("    ... (DLL 过多，截断)\r\n", CMD_DIM);
                    break;
                }
            }
            term_puts("  共 ");
            term_putu(dll_count);
            term_puts(" 个 DLL, ");
            term_putu(func_total);
            term_puts(" 个导入函数\r\n");
        }
    } else {
        term_puts_color("  导入表: 无\r\n", CMD_DIM);
    }

    term_puts_color("=== PE 头信息结束 ===\r\n", CMD_INFO);
}

/* ============================================================
 *  pe — 显式运行 PE 程序（等价于直接输入名字，但可在 help 中列出）
 * ============================================================ */
static void cmd_pe(const char *args) {
    if (!*args) {
        term_puts_color("用法: pe <程序名>  (如 pe hello64)\r\n", CMD_INFO);
        return;
    }
    exec_pe(args, args);
}

/* ============================================================
 *  命令解析与分发
 * ============================================================ */

static int g_should_exit = 0;

/* driver_init 注入（正式定义在文件后段，电源分发先用到） */
static const struct dkm_kernel_api *g_cmd_api;

/* ---- 电源：经 acpi.drv 的服务（srv.acpi.poweroff / srv.acpi.reboot）。
 * 服务缺席（无 ACPI 表 / FUCK [drivers] acpi=0）时退回本文件原有行为：
 * 关机 = 停机自旋；重启 = 键盘控制器 + 0xCF9 + 三击。 ---- */
static void *cmd_svc(const char *name) {
    void *fn = 0;
    if (!g_cmd_api || !g_cmd_api->bus) return 0;
    if (g_cmd_api->bus->probe(name, &fn, 0) != 0) return 0;
    return fn;
}

static void cmd_power_off(void) {
    int (*svc)(void) = (int (*)(void))cmd_svc("srv.acpi.poweroff");
    if (svc) svc();               /* 成功不返回 */
    term_puts_color("系统已停止（ACPI S5 不可用）\r\n", CMD_DIM);
    term_redraw_all();
    for (;;) __asm__ volatile("hlt");
}

static void cmd_power_reboot(void) {
    int (*svc)(void) = (int (*)(void))cmd_svc("srv.acpi.reboot");
    if (svc) svc();               /* 成功不返回 */
    for (u64 i = 0; i < 100000000ULL; i++) __asm__ volatile("pause");
    outb(0x64, 0xFE);
    outb(0xCF9, 0x06);
    __asm__ volatile("int $0x03");   /* 三击复位 */
}

static void execute_command(const char *cmd) {
    while (*cmd == ' ') cmd++;
    if (*cmd == 0) return;

    /* 提取命令名 */
    const char *args = cmd;
    while (*args && *args != ' ') args++;
    int cmd_name_len = (int)(args - cmd);
    char name[64];
    if (cmd_name_len >= 64) cmd_name_len = 63;
    for (int i = 0; i < cmd_name_len; i++) name[i] = cmd[i];
    name[cmd_name_len] = 0;
    while (*args == ' ') args++;

    /* 内置命令（不区分大小写） */
    if (str_ieq(name, "help") || str_eq(name, "?")) { cmd_help(); return; }
    if (str_ieq(name, "ver")) { cmd_ver(); return; }
    if (str_ieq(name, "cls") || str_ieq(name, "clear")) { cmd_cls(); return; }
    if (str_ieq(name, "dir") || str_ieq(name, "ls")) { cmd_dir(); return; }
    if (str_ieq(name, "cd")) { cmd_cd(); return; }
    if (str_ieq(name, "type") || str_ieq(name, "cat")) { cmd_type(args); return; }
    if (str_ieq(name, "copy") || str_ieq(name, "cp")) { cmd_copy(args); return; }
    if (str_ieq(name, "del") || str_ieq(name, "rm")) { cmd_del(args); return; }
    if (str_ieq(name, "ren") || str_ieq(name, "mv")) { cmd_ren(args); return; }
    if (str_ieq(name, "mkdir") || str_ieq(name, "md")) { cmd_mkdir(); return; }
    if (str_ieq(name, "rmdir") || str_ieq(name, "rd")) { cmd_rmdir(); return; }
    if (str_ieq(name, "echo")) { cmd_echo(args); return; }
    if (str_ieq(name, "set")) { cmd_set(); return; }
    if (str_ieq(name, "exit") || str_ieq(name, "quit")) {
        term_puts_color("返回桌面...\r\n", CMD_DIM);
        g_should_exit = 1;
        return;
    }
    if (str_ieq(name, "pe"))     { cmd_pe(args);     return; }
    if (str_ieq(name, "peinfo")) { cmd_peinfo(args); return; }
    if (str_ieq(name, "shutdown") || str_ieq(name, "poweroff") || str_ieq(name, "halt")) {
        term_puts_color("关机中...\r\n", CMD_WARN);
        term_redraw_all();
        cmd_power_off();          /* ACPI S5（srv.acpi.poweroff）；失败则停机 */
        return;
    }
    if (str_ieq(name, "reboot")) {
        term_puts_color("重启中...\r\n", CMD_WARN);
        term_redraw_all();
        cmd_power_reboot();       /* ACPI reset / 0xCF9 / 三击 */
        return;
    }

    /* PE/EXE 执行：检查是否为可执行文件 */
    int ext_type = has_exec_ext(name);
    if (ext_type == 1) {
        /* .exe/.com → PE 执行 */
        exec_pe(name, cmd);
        return;
    }
    if (ext_type == 2) {
        /* .bat → 批处理（暂不支持） */
        term_puts_color("批处理文件 (.bat) 暂不支持\r\n", CMD_WARN);
        return;
    }
    /* 无扩展名 → 尝试自动补 .exe 执行 */
    if (cmd_name_len > 0 && name[0] != '.') {
        exec_pe(name, cmd);
        return;
    }

    /* 未知命令 */
    term_puts_color("'", CMD_ERROR);
    term_puts_color(name, CMD_ERROR);
    term_puts_color("' 不是内部或外部命令，也不是可运行的程序。\r\n", CMD_ERROR);
}

/* ============================================================
 *  AUTOEXEC.BAT — 启动时自动执行根目录下的批处理（每行一条命令）
 *  空行与以 :: 或 rem 开头的行跳过。
 * ============================================================ */
static void run_autoexec(void) {
    if (!f32_blk_read) return;
    char name11[11];
    if (f32_name_to_83("AUTOEXEC.BAT", name11) != 0) return;
    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file(name11, &data, &size) != 0) {
        logl("[cmd] no AUTOEXEC.BAT");
        return;
    }
    logl("[cmd] running AUTOEXEC.BAT");
    term_puts_color("执行 AUTOEXEC.BAT ...\r\n", CMD_DIM);

    u32 pos = 0;
    while (pos < size && !g_should_exit) {
        /* 取一行 */
        char line[128];
        int li = 0;
        while (pos < size && data[pos] != '\n' && data[pos] != '\r' && li < 127) {
            line[li++] = (char)data[pos++];
        }
        while (pos < size && (data[pos] == '\n' || data[pos] == '\r')) pos++;
        line[li] = 0;
        /* 去前导空格 */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        if (p[0] == ':' && p[1] == ':') continue;
        if ((p[0] == 'r' || p[0] == 'R') && (p[1] == 'e' || p[1] == 'E')
            && (p[2] == 'm' || p[2] == 'M') && (p[3] == ' ' || p[3] == 0)) continue;

        /* 回显并执行 */
        term_puts_color(PROMPT, CMD_PROMPT);
        term_puts(p);
        term_puts("\r\n");
        term_redraw_all();
        execute_command(p);
    }
}

/* ============================================================
 *  主入口
 * ============================================================ */

/* ===================================================================
 *  P4f 万物互联：DKM 驱动入口。driver_init 从 api/bus 解析服务并注册
 *  srv.cmd.run；cmd_run 执行 CMD 主循环（Esc/AUTOEXIT 可返回）。
 *  同源构建的 cmd.elf 仍走下方 dsk_entry（desktop 开始菜单启动）。
 * =================================================================== */
static struct dsk_boot_context g_cmd_compat_ctx;
static int cmd_run(void);   /* 前向声明 */

/* 外部 .elf 入口：desktop 开始菜单/桌面图标仍以 dsk_boot_context ABI 启动
 * cmd.elf（与 cmd.drv 同源）。P4f 化时 driver_init 取代了 dsk_entry，导致
 * cmd.elf 链接找不到入口符号、e_entry=0 —— 点击 CMD 会跳到镜像前一页的
 * 垃圾地址。此入口按 compat 上下文建立与 driver_init 相同的运行环境，
 * 再进入 cmd_run。 */
__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    __asm__ volatile("cli");
    if (!ctx || ctx->magic != 0x44534B31424F4F54ULL) {
        logl("[cmd] bad context");
        for (;;) __asm__ volatile("hlt");
    }

    const struct dkm_kernel_api *api =
        (const struct dkm_kernel_api *)ctx->dkm_kernel_api;
    if (!api) {
        logl("[cmd] no kernel api");
        for (;;) __asm__ volatile("hlt");
    }

    g_cmd_api = api;
    g_kernel_api = (u64)api;
    g_boot_ctx = ctx;
    fb_a = ctx->framebuffer_address;
    fb_w = ctx->framebuffer_width;
    fb_h = ctx->framebuffer_height;
    fb_p = ctx->framebuffer_pitch;

    cmd_f32_init_from_api(api);

    /* PE 服务：desktop 的 compat 上下文已把总线取得的服务回填 reserved[4]；
     * 缺失时回落总线查询（与 driver_init 同路径）。 */
    g_pe_svc = (const pe_service *)ctx->reserved[4];
    if (!g_pe_svc) {
        void *fn = 0;
        if (api->bus && api->bus->probe("kern.pe.service", &fn, 0) == 0 && fn) {
            const pe_service *(*pget)(void) = (const pe_service *(*)(void))fn;
            const pe_service *pe = pget();
            if (pe && pe->magic == PE_SERVICE_MAGIC) g_pe_svc = pe;
        }
    }

    (void)cmd_run();
}

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api, struct dkm_driver_handle *handle) {
    (void)handle;
    if (!api || !api->log) return -1;
    g_cmd_api = api;
    g_kernel_api = (u64)api;

    fb_a = (u64)api->fb_address;
    fb_w = api->fb_width;
    fb_h = api->fb_height;
    fb_p = api->fb_pitch;

    /* block（P4f：api->block 直读，不再 +0xA8 裸偏移）+ 启动卷设备重定向 */
    cmd_f32_init_from_api(api);

    /* PE 服务（P4f：经总线 kern.pe.service，不再 reserved[4]） */
    g_pe_svc = 0;
    {
        void *fn = 0;
        if (api->bus && api->bus->probe("kern.pe.service", &fn, 0) == 0 && fn) {
            const pe_service *(*pget)(void) = (const pe_service *(*)(void))fn;
            const pe_service *pe = pget();
            if (pe && pe->magic == PE_SERVICE_MAGIC) g_pe_svc = pe;
        }
    }

    /* 兼容 boot-context（供子应用，P5 后可移除） */
    g_cmd_compat_ctx.magic = 0x44534B31424F4F54ULL;
    g_cmd_compat_ctx.framebuffer_address = (u64)api->fb_address;
    g_cmd_compat_ctx.framebuffer_width = api->fb_width;
    g_cmd_compat_ctx.framebuffer_height = api->fb_height;
    g_cmd_compat_ctx.framebuffer_pitch = api->fb_pitch;
    g_cmd_compat_ctx.dkm_kernel_api = (u64)api;
    g_boot_ctx = &g_cmd_compat_ctx;

    if (api->bus) {
        api->bus->register_service("srv.cmd.run", 1, (void *)cmd_run);
        api->log->info("[cmd] srv.cmd.run registered");
    }
    return 0;
}

static int host_take_close(void) {
    const desk_host_view *v;
    dkm_input_mouse m;
    if (!g_boot_ctx || !(g_boot_ctx->flags & DSK_BOOT_FLAG_HOSTED)) return 0;
    v = (const desk_host_view *)(u64)g_boot_ctx->kernel_stack_top;
    if (!v || v->magic != DESK_HOST_MAGIC) return 0;
    if (v->pump) v->pump();
    if (!g_input.mouse_poll || v->close_w == 0) return 0;
    if (g_input.mouse_poll(&m) != 0 || m.magic != DKM_INPUT_MOUSE_MAGIC) return 0;
    if ((m.buttons & 1u) == 0) return 0;
    return m.x >= (int)v->close_x && m.x < (int)v->close_x + (int)v->close_w &&
           m.y >= (int)v->close_y && m.y < (int)v->close_y + (int)v->close_h;
}

static int cmd_run(void) {
    const struct dkm_kernel_api *api = g_cmd_api;
    logl("[cmd] boot");

    if (dkm_input_open(&g_input, api) != 0) {
        g_cmd_api->log->error("[cmd] srv.input.kbd.poll missing");
        g_cmd_api->log->panic("INPUT-E01 KBD POLL SERVICE MISSING");
        return 0;
    }
    __asm__ volatile("sti" ::: "memory");   /* IRQ1/IRQ12 已由 session.drv 解屏蔽 */
    logl("[cmd] input: kbd via srv.input.kbd.poll");

    if (f32_blk_read)  logl("[cmd] block_read ok");
    if (f32_blk_write) logl("[cmd] block_write ok");
    if (g_pe_svc) logl("[cmd] PE service ok");
    else logl("[cmd] PE service not available");

    /* dev_mode 串口镜像（P4f：经 kern.config.get，不再 reserved[3]） */
    {
        void *fn = 0;
        if (api && api->bus && api->bus->probe("kern.config.get", &fn, 0) == 0 && fn) {
            const char *(*get)(const char *, const char *) = (const char *(*)(const char *, const char *))fn;
            const char *dm = get("boot", "dev_mode");
            if (dm && dm[0] == '1') g_serial_mirror = 1;
        }
    }

    /* 初始化渲染上下文 */
    du_context_init(&g_ctx, fb_a, fb_w, fb_h, fb_p);
    term_init_pos();
    term_clear_buf();
    term_redraw_all();

    /* 启动 banner */
    term_puts_color("Deshab CMD [Version 0.1.0]\r\n", CMD_OK);
    term_puts_color("(c) 2026 Deshab Project. PE/EXE 兼容层 by UTSM.\r\n", CMD_DIM);
    term_putc('\r');
    term_putc('\n');
    if (g_pe_svc) {
        term_puts_color("PE 兼容层已就绪 (PE32+ 原生 + PE32 解释器)\r\n", CMD_OK);
    } else {
        term_puts_color("警告: PE 兼容层不可用\r\n", CMD_WARN);
    }
    term_putc('\r');
    term_putc('\n');
    term_redraw_all();

    /* AUTOEXEC.BAT 自动执行（存在则逐行运行） */
    run_autoexec();
    if (g_should_exit) return 0;

    /* 主循环 */
    int shift = 0;
    int e0 = 0;
    u64 blink_start = rdtsc_cmd();
    g_should_exit = 0;

    for (;;) {
        /* 绘制提示符 */
        term_puts_color(PROMPT, CMD_PROMPT);
        input_len = 0;
        input_cursor = 0;
        input_buf[0] = 0;
        int prompt_len = str_len(PROMPT);
        cur_col = prompt_len;
        cur_visible = 1;
        redraw_input_line();

        int cmd_done = 0;
        while (!cmd_done && !g_should_exit) {
            /* 光标闪烁 */
            if (g_tsc_per_ms) {
                u64 now = rdtsc_cmd();
                u64 elapsed = now - blink_start;
                int want_vis = (elapsed / (g_tsc_per_ms * 530)) & 1;
                if (want_vis != cur_visible) {
                    if (want_vis) term_redraw_cursor();
                    else term_clear_cursor();
                    cur_visible = want_vis;
                }
            }

            u8 sc;
            if (host_take_close()) {
                logl("[cmd] close -> return to DSK");
                g_should_exit = 1;
                break;
            }
            if (!dkm_input_kbd(&g_input, &sc)) { __asm__("pause"); continue; }
            if (!g_input_logged) {
                g_input_logged = 1;
                logl("[cmd] kbd event via srv.input.kbd.poll");
            }

            blink_start = rdtsc_cmd();
            if (!cur_visible) { cur_visible = 1; term_redraw_cursor(); }

            if (sc == 0xE0) { e0 = 1; continue; }
            if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }

            if (sc & 0x80) {
                /* 按键释放 */
                e0 = 0;
                continue;
            }

            if (e0) {
                if (sc == 0x4B) cursor_left();
                else if (sc == 0x4D) cursor_right();
                else if (sc == 0x47) cursor_home();
                else if (sc == 0x4F) cursor_end();
                else if (sc == 0x53) { /* Delete */ }
                e0 = 0;
                continue;
            }

            if (sc == 0x1C) {
                /* Enter — 提交命令 */
                term_clear_cursor();
                input_buf[input_len] = 0;
                term_putc('\r');
                term_putc('\n');
                execute_command(input_buf);
                input_len = 0;
                input_cursor = 0;
                input_buf[0] = 0;
                cmd_done = 1;
                term_redraw_all();
                continue;
            }

            if (sc == 0x0E) { delete_char_back(); continue; }
            if (sc == 0x01) {
                /* Esc — 返回桌面 */
                logl("[cmd] esc -> return to DSK");
                g_should_exit = 1;
                cmd_done = 1;
                continue;
            }
            if (sc == 0x15 && !shift) { kill_line(); continue; }

            char c = scan_to_ascii(sc, shift);
            if (c && c >= 32 && c <= 126) {
                insert_char(c);
            }
        }

        if (g_should_exit) break;
    }

    logl("[cmd] exit");
    return 0;
}

/* ===================================================================
 *  P4f: DKM 驱动描述符 + 退出
 * =================================================================== */
static const char *const g_provides[] = { "cmd" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic = DKM_DRIVER_MAGIC, .abi_version = DKM_ABI_VERSION,
    .desc_size = sizeof(struct dkm_driver_desc),
    .name = "cmd", .version = "0.1.0", .vendor = "Deshab",
    .driver_class = DKM_CLASS_MISC, .stage = 4,
    .flags = DKM_F_NO_UNLOAD, .priority = 0,
    .depends = NULL, .depends_count = 0,
    .provides = g_provides, .provides_count = 1,
    .min_kernel_abi = 1,
};

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle; return 0;
}
