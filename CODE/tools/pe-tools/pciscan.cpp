/* pciscan.cpp — Deshab PCI/CPU 扫描（控制台 PE64 工具）
 *
 * 在 CMD 里敲 "pciscan" 运行（cmd.elf PATH：FAT32 根 → bin/）。
 * 输出走串口（shim putchar/puts → COM1；QEMU -serial stdio 可见）。
 * 注意 shim printf 仅支持 3 个格式参数 —— 全部输出用自制助手拼行。
 *
 * 内容：CPUID 厂商/型号串/核心数/hypervisor 位 + PCI 配置空间枚举
 * （bus 0-3，0xCF8/0xCFC 直读，Ring0 原生执行）+ RTC 日期时间。
 */
#include <windows.h>

typedef unsigned int   u32;
typedef unsigned char  u8;
typedef unsigned short u16;

static inline void outp(u16 port, u32 v) {
    __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(port));
}
static inline u32 inp(u16 port) {
    u32 v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline u8 inpb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outpb(u16 port, u8 v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(port));
}

/* ---- 输出助手（shim printf 参数受限，逐段 puts/putchar 拼行） ---- */
static void ps(const char *s) { while (*s) putchar(*s++); }
static void pch(char c) { putchar(c); }
static void pd(u32 v) {
    char b[12];
    int n = 0;
    if (v == 0) { pch('0'); return; }
    while (v > 0) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) pch(b[--n]);
}
static void ph(u32 v) {
    static const char hd[] = "0123456789ABCDEF";
    ps("0x");
    for (int i = 7; i >= 0; i--) pch(hd[(v >> (i * 4)) & 0xF]);
}
static void ph4(u32 v) {
    static const char hd[] = "0123456789ABCDEF";
    for (int i = 3; i >= 0; i--) pch(hd[(v >> (i * 4)) & 0xF]);
}

static u32 pci_read(u16 bus, u16 dev, u16 fn, u8 off) {
    outp(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11)
              | ((u32)fn << 8) | off);
    return inp(0xCFC);
}

static void cpuid_(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                             : "a"(leaf), "c"(0));
}

static int bcd(u8 v) { return (v >> 4) * 10 + (v & 0xF); }

int main(void) {
    /* ---- CPU ---- */
    u32 a, b, c, d;
    cpuid_(0, &a, &b, &c, &d);
    char vendor[13];
    ((u32 *)vendor)[0] = b; ((u32 *)vendor)[1] = d; ((u32 *)vendor)[2] = c;
    vendor[12] = 0;
    ps("Deshab pciscan\r\n");
    ps("CPU vendor: "); ps(vendor); ps("\r\n");
    ps("CPUID max leaf: "); ph(a); ps("\r\n");

    char brand[49];
    brand[0] = 0;
    if (a >= 0x80000004u) {
        for (u32 leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
            cpuid_(leaf, &a, &b, &c, &d);
            int off = (int)(leaf - 0x80000002u) * 16;
            ((u32 *)brand)[off / 4 + 0] = a;
            ((u32 *)brand)[off / 4 + 1] = b;
            ((u32 *)brand)[off / 4 + 2] = c;
            ((u32 *)brand)[off / 4 + 3] = d;
        }
        brand[48] = 0;
        ps("CPU brand: "); ps(brand); ps("\r\n");
    }

    cpuid_(1, &a, &b, &c, &d);
    ps("CPU cores (max APIC id): "); pd((b >> 16) & 0xFF); ps("\r\n");
    ps("Hypervisor: "); ps(((c >> 31) & 1) ? "yes" : "no"); ps("\r\n");

    /* ---- PCI ---- */
    ps("\r\nPCI devices (bus 0-3):\r\n");
    int found = 0;
    for (u16 bus = 0; bus < 4; bus++) {
        for (u16 dev = 0; dev < 32; dev++) {
            for (u16 fn = 0; fn < 8; fn++) {
                u32 vd = pci_read(bus, dev, fn, 0);
                u16 ven = vd & 0xFFFF, dev_id = vd >> 16;
                if (ven == 0xFFFF) continue;
                u32 cl = pci_read(bus, dev, fn, 8);
                u32 ht = pci_read(bus, dev, fn, 12);
                ps("  ");
                pd(bus); pch(':'); pd(dev); pch('.'); pch((char)('0' + fn));
                ps("  ");
                ph4(ven); pch(':'); ph4(dev_id);
                ps("  class "); ph4((cl >> 16) & 0xFFFF);
                ps("  rev ");   ph4(cl & 0xFF);
                pch('\r'); pch('\n');
                found++;
                /* 非 multifunction 设备跳过 fn>0 */
                if (fn == 0 && !((ht >> 16) & 0x80)) break;
            }
        }
    }
    ps("Total PCI functions: "); pd((u32)found); ps("\r\n");

    /* ---- RTC ---- */
    outpb(0x70, 0x09); int yy = bcd(inpb(0x71));
    outpb(0x70, 0x08); int mo = bcd(inpb(0x71));
    outpb(0x70, 0x07); int dd = bcd(inpb(0x71));
    outpb(0x70, 0x04); int hh = bcd(inpb(0x71));
    outpb(0x70, 0x02); int mi = bcd(inpb(0x71));
    outpb(0x70, 0x00); int ss = bcd(inpb(0x71));
    if (yy < 100) yy += 2000;
    ps("RTC: "); pd((u32)yy); pch('-'); pd((u32)mo); pch('-'); pd((u32)dd);
    pch(' '); pd((u32)hh); pch(':'); pd((u32)mi); pch(':'); pd((u32)ss);
    ps("\r\n");
    ps("done.\r\n");
    ExitProcess(0);
    return 0;
}
