/* portio.cpp — Deshab 端口读写工具（控制台 PE64，diskpart 风格硬件调试）
 *
 * 用法（在 CMD 里运行，输出走串口）：
 *   portio <port>            读一字节（十六进制端口）
 *   portio <port> <value>    写一字节
 *   portio                   用法帮助
 *
 * 例：
 *   portio 70        → 读 CMOS 地址口
 *   portio 70 0      → CMOS 寄存器 0（秒）
 *   portio 71        → 读出秒（BCD）
 *
 * Ring0 原生执行，IOPL 无限制。危险提示：写错端口可能挂机。
 */
#include <windows.h>

typedef unsigned int   u32;
typedef unsigned char  u8;
typedef unsigned short u16;

static inline u8 inpb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outpb(u16 port, u8 v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(port));
}

static void ps(const char *s) { while (*s) putchar(*s++); }
static void pch(char c) { putchar(c); }
static void ph(u32 v) {
    static const char hd[] = "0123456789ABCDEF";
    ps("0x");
    for (int i = 3; i >= 0; i--) pch(hd[(v >> (i * 4)) & 0xF]);
}

/* 解析十进制/0x 前缀十六进制；成功返回 0 */
static int parse_num(const char *s, u32 *out) {
    u32 v = 0;
    if (!s || !s[0]) return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!s[0]) return -1;
        for (;;) {
            char c = *s++;
            if (!c) break;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return -1;
            v = v * 16 + (u32)d;
        }
    } else {
        for (;;) {
            char c = *s++;
            if (!c) break;
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (u32)(c - '0');
        }
    }
    *out = v;
    return 0;
}

/* 从 cmdline 提取第 n 个空格分隔 token（0 = 程序名） */
static int token(const char *cmd, int n, char *out, int cap) {
    int idx = 0, ti = 0;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        ti = 0;
        while (*p && *p != ' ') {
            if (idx == n && ti < cap - 1) out[ti++] = *p;
            p++;
        }
        if (idx == n) {
            out[ti] = 0;
            return ti > 0 ? 0 : -1;
        }
        idx++;
    }
    return -1;
}

int main(void) {
    const char *cmd = GetCommandLineA();
    char t1[32], t2[32];
    u32 port, val;

    if (token(cmd, 1, t1, sizeof(t1)) != 0) {
        ps("Deshab portio - port read/write tool (serial console)\r\n");
        ps("usage: portio <port> [value]   (hex with 0x, or decimal)\r\n");
        ps("  portio 70        read CMOS address port\r\n");
        ps("  portio 70 0      select CMOS reg 0 (seconds)\r\n");
        ps("  portio 71        read seconds (BCD)\r\n");
        ps("WARNING: wrong port writes can hang the machine.\r\n");
        ExitProcess(0);
    }
    if (parse_num(t1, &port) != 0 || port > 0xFFFF) {
        ps("portio: bad port\r\n");
        ExitProcess(1);
    }

    if (token(cmd, 2, t2, sizeof(t2)) == 0) {
        /* 写 */
        if (parse_num(t2, &val) != 0 || val > 0xFF) {
            ps("portio: bad value\r\n");
            ExitProcess(1);
        }
        outpb((u16)port, (u8)val);
        ps("portio: wrote "); ph(val); ps(" -> port "); ph(port); ps("\r\n");
    } else {
        u8 v = inpb((u16)port);
        ps("portio: port "); ph(port); ps(" = "); ph(v); ps("\r\n");
    }
    ExitProcess(0);
    return 0;
}
