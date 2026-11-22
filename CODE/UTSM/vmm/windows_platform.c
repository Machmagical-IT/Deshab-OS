#include <utsm/windows_guest.h>
#include <utsm/vmx.h>
#include <utsm/vmm.h>
#include <utsm/ept.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

#define PCI_CFG_ENABLE 0x80000000u

struct windows_pc_state {
    u32 pci_addr;
    u8 cmos_index;
    u16 fw_cfg_selector;
    u32 fw_cfg_offset;
    u32 ioapic_select;
    u32 ioapic_id;
    u64 ioapic_redir[24];
    u32 lapic_svr[WINDOWS_VCPU_COUNT];
    u32 lapic_lvt_timer[WINDOWS_VCPU_COUNT];
    u32 lapic_initial_count[WINDOWS_VCPU_COUNT];
    u32 lapic_current_count[WINDOWS_VCPU_COUNT];
    u32 lapic_divide[WINDOWS_VCPU_COUNT];
    u32 lapic_icr_high[WINDOWS_VCPU_COUNT];
    u32 lapic_tpr[WINDOWS_VCPU_COUNT];
    u32 lapic_isr[WINDOWS_VCPU_COUNT][8];
    u64 lapic_last_tsc[WINDOWS_VCPU_COUNT];
    u64 lapic_base[WINDOWS_VCPU_COUNT];
    u64 hpet_config;
    u64 hpet_counter_base;
    u64 hpet_tsc_base;
    u32 pci_mch[64];
    u32 pci_lpc[64];
    u8 pm_io[16];
    u32 vblk_bar;
    u32 vblk_cmd;
    u32 vblk_pfn;
    u32 vblk_driver_feat;
    u16 vblk_last_avail;
    u8 vblk_status;
    u8 vblk_isr;
    u8 vblk_iline;
    int initialized;
    int irq_armed;
};

static struct windows_pc_state g_pc;
extern int windows_acpi_build(void);
static void windows_log_sleep_caller(void);
static u64 windows_gva_to_gpa(u64 gva);

static u64 rdtsc_local(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void zero_bytes(void *ptr, u64 size) {
    u8 *p = (u8 *)ptr;
    for (u64 i = 0; i < size; i++) p[i] = 0;
}

static u32 *pci_config_slot(u8 dev, u8 fn) {
    if (dev == 0 && fn == 0) return g_pc.pci_mch;
    if (dev == 0x1F && fn == 0) return g_pc.pci_lpc;
    return (u32 *)0;
}

static u32 pci_config_dword(u8 bus, u8 dev, u8 fn, u8 reg) {
    if (bus == 0 && dev == 3 && fn == 0) {
        static u32 seen;
        if (seen < 4) {
            seen++;
            log_hex64("[WIN] pci virtio cfg=", ((u64)reg << 32) | seen);
        }
        switch (reg) {
        case 0x00: return 0x10011AF4u; /* virtio-blk legacy */
        case 0x04: return g_pc.vblk_cmd;
        case 0x08: return 0x01000000u; /* mass storage */
        case 0x10: return (g_pc.vblk_bar & 0xFFFFFFC0u) | 1u;
        case 0x2C: return 0x00021AF4u; /* subsystem: virtio block */
        case 0x3C: return 0x00000100u | g_pc.vblk_iline;
        default: return 0;
        }
    }
    u32 *slot = pci_config_slot(dev, fn);
    if (bus != 0 || !slot) return 0xFFFFFFFFu;
    return slot[reg >> 2];
}

static void pci_config_write(u8 bus, u8 dev, u8 fn, u8 reg, u32 value) {
    if (bus == 0 && dev == 3 && fn == 0) {
        if (reg == 0x04) g_pc.vblk_cmd = value & 0xFFFFu;
        else if (reg == 0x10) g_pc.vblk_bar = value;
        else if (reg == 0x3C) g_pc.vblk_iline = (u8)value;
        return;
    }
    u32 *slot = pci_config_slot(dev, fn);
    if (bus != 0 || !slot) return;
    slot[reg >> 2] = value;
}

#define WIN_VBLK_QSZ 64u

static u32 g_disk_reads;

u32 windows_platform_disk_reads(void) {
    return g_disk_reads;
}

static int gpa_copy(u64 gpa, u8 *host, u32 len, int to_guest) {
    u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
    if (!hhdm || !host) return -1;
    while (len) {
        u32 chunk = (u32)(0x1000u - (gpa & 0xFFFu));
        if (chunk > len) chunk = len;
        u64 hpa = ept_gpa_to_hpa(gpa & ~0xFFFULL);
        if (!hpa) return -1;
        u8 *page = (u8 *)(hhdm + hpa + (gpa & 0xFFFu));
        for (u32 i = 0; i < chunk; i++) {
            if (to_guest) page[i] = host[i];
            else host[i] = page[i];
        }
        host += chunk;
        gpa += chunk;
        len -= chunk;
    }
    return 0;
}

static void virtio_blk_complete(void) {
    const struct windows_guest_info *info = windows_guest_get_info();
    if (!info || !info->esp_image || !g_pc.vblk_pfn) return;
    u64 qbase = (u64)g_pc.vblk_pfn << 12;
    u64 avail = qbase + WIN_VBLK_QSZ * 16u;
    u16 avail_idx = 0;
    if (gpa_copy(avail + 2, (u8 *)&avail_idx, 2, 0) != 0) return;
    u8 *image = (u8 *)info->esp_image;
    static u32 reads;
    while (g_pc.vblk_last_avail != avail_idx) {
        u16 slot = g_pc.vblk_last_avail % WIN_VBLK_QSZ;
        u16 head = 0;
        if (gpa_copy(avail + 4 + (u64)slot * 2, (u8 *)&head, 2, 0) != 0) return;
        u8 hdr[16];
        u32 hdr_len = 0;
        u64 frag_gpa[8];
        u32 frag_len[8];
        u8 frag_wr[8];
        u32 frags = 0;
        u64 status_gpa = 0;
        u16 idx = head;
        for (u32 i = 0; i < 16; i++) hdr[i] = 0;
        for (u32 n = 0; n < 8 && frags <= 8; n++) {
            u8 raw[16];
            if (gpa_copy(qbase + (u64)idx * 16u, raw, 16, 0) != 0) return;
            u64 addr = 0;
            u32 len = 0;
            u16 flags = 0, next = 0;
            for (u32 b = 0; b < 8; b++) addr |= (u64)raw[b] << (b * 8);
            for (u32 b = 0; b < 4; b++) len |= (u32)raw[8 + b] << (b * 8);
            flags = (u16)(raw[12] | (raw[13] << 8));
            next = (u16)(raw[14] | (raw[15] << 8));
            int wr = (flags & 2) != 0;
            if (!wr && hdr_len < 16 && len) {
                u32 take = 16 - hdr_len;
                if (take > len) take = len;
                if (gpa_copy(addr, hdr + hdr_len, take, 0) != 0) return;
                hdr_len += take;
            } else if (wr && len == 1 && !status_gpa) {
                status_gpa = addr;
            } else if (len) {
                if (frags < 8) {
                    frag_gpa[frags] = addr;
                    frag_len[frags] = len;
                    frag_wr[frags] = (u8)wr;
                    frags++;
                } else {
                    frags = 9;
                }
            }
            if (!(flags & 1)) break;
            idx = next;
        }
        u8 status = 2;
        u32 type = (u32)hdr[0] | ((u32)hdr[1] << 8) |
                   ((u32)hdr[2] << 16) | ((u32)hdr[3] << 24);
        u64 sector = 0;
        for (u32 b = 0; b < 8; b++) sector |= (u64)hdr[8 + b] << (b * 8);
        u64 off = sector * 512u;
        u32 data_len = 0;
        int data_write = frags > 0 && frags <= 8;
        if (data_write) {
            for (u32 f = 0; f < frags; f++) {
                data_len += frag_len[f];
                if (type == 0 && !frag_wr[f]) data_write = 0;
                if (type == 1 && frag_wr[f]) data_write = 0;
            }
        }
        if (hdr_len >= 16 && frags <= 8 &&
            off + data_len <= info->esp_image_size) {
            if (type == 0 && data_write) {
                u64 cursor = off;
                int copied = 1;
                for (u32 f = 0; f < frags; f++) {
                    if (gpa_copy(frag_gpa[f], image + cursor,
                                 frag_len[f], 1) != 0) {
                        copied = 0;
                        break;
                    }
                    cursor += frag_len[f];
                }
                if (copied) {
                    status = 0;
                    reads++;
                    g_disk_reads = reads;
                    if (reads <= 24 || reads == 32 || reads == 64 ||
                        reads == 256 || reads == 1024) {
                        log_hex64("[WIN] disk n=", reads);
                        log_hex64("[WIN] disk lba=", sector);
                        log_hex64("[WIN] disk len=", data_len);
                    }
                    if (sector == 0 && frags && frag_len[0] >= 16) {
                        static int sig;
                        if (!sig) {
                            sig = 1;
                            u64 word = 0;
                            for (int i = 0; i < 8; i++)
                                word |= (u64)image[i] << (i * 8);
                            log_hex64("[WIN] disk sig=", word);
                            u8 bpb[8];
                            if (gpa_copy(frag_gpa[0] + 11, bpb, 8, 0) == 0) {
                                u64 w = 0;
                                for (int b = 0; b < 8; b++)
                                    w |= (u64)bpb[b] << (b * 8);
                                log_hex64("[WIN] disk bpb=", w);
                            }
                        }
                    }
                }
            } else if (type == 1 && data_write) {
                u64 cursor = off;
                int copied = 1;
                for (u32 f = 0; f < frags; f++) {
                    if (gpa_copy(frag_gpa[f], image + cursor,
                                 frag_len[f], 0) != 0) {
                        copied = 0;
                        break;
                    }
                    cursor += frag_len[f];
                }
                if (copied) status = 0;
            } else if ((type == 4 || type == 8) && data_len == 0) {
                status = 0;
            } else if (type == 8 && data_write && frags) {
                static const u8 id[20] = "DESHABWIN-ESP";
                u32 left = 20;
                for (u32 f = 0; f < frags && left; f++) {
                    u32 n = frag_len[f] < left ? frag_len[f] : left;
                    if (gpa_copy(frag_gpa[f], (u8 *)id + (20 - left), n, 1) != 0)
                        break;
                    left -= n;
                }
                status = 0;
            } else if (type == 4) {
                status = 0;
            }
        }
        if (status != 0) {
            static u32 fails;
            if (fails < 12) {
                fails++;
                log_hex64("[WIN] disk fail n=", fails);
                log_hex64("[WIN] disk fail type=", type);
                log_hex64("[WIN] disk fail lba=", sector);
                log_hex64("[WIN] disk fail meta=",
                          ((u64)hdr_len << 32) | ((u64)frags << 24) | data_len);
            }
        }
        if (status_gpa) gpa_copy(status_gpa, &status, 1, 1);
        u64 used = (avail + 4 + (u64)WIN_VBLK_QSZ * 2 + 4095) & ~4095ULL;
        u16 used_idx = 0;
        gpa_copy(used + 2, (u8 *)&used_idx, 2, 0);
        u8 ent[8];
        for (u32 b = 0; b < 8; b++) ent[b] = 0;
        ent[0] = (u8)head;
        ent[1] = (u8)(head >> 8);
        u32 wrote = status == 0 ? data_len : 0;
        for (u32 b = 0; b < 4; b++) ent[4 + b] = (u8)(wrote >> (b * 8));
        gpa_copy(used + 4 + (u64)(used_idx % WIN_VBLK_QSZ) * 8, ent, 8, 1);
        used_idx++;
        gpa_copy(used + 2, (u8 *)&used_idx, 2, 1);
        g_pc.vblk_last_avail++;
        g_pc.vblk_isr |= 1;
    }
}

static int virtio_blk_io(u16 port, int is_in, u32 size, u64 value, u64 *result) {
    u32 base = g_pc.vblk_bar & 0xFFFFFFC0u;
    if (!(g_pc.vblk_cmd & 1) || !base) return 0;
    if (port < base || port >= base + 0x40) return 0;
    u16 off = (u16)(port - base);
    if (off == 0x00 && is_in) *result = 0; /* no optional features */
    else if (off == 0x04 && !is_in) g_pc.vblk_driver_feat = (u32)value;
    else if (off == 0x08 && !is_in) g_pc.vblk_pfn = (u32)value;
    else if (off == 0x0C && is_in) *result = WIN_VBLK_QSZ;
    else if (off == 0x12 && is_in) *result = g_pc.vblk_status;
    else if (off == 0x12 && !is_in) {
        g_pc.vblk_status = (u8)value;
        if (value == 0) g_pc.vblk_last_avail = 0;
        if (value & 4) log_info("[WIN] virtio-blk driver ok");
    } else if (off == 0x13 && is_in) {
        *result = g_pc.vblk_isr;
        g_pc.vblk_isr = 0;
    } else if (off == 0x10 && !is_in) {
        virtio_blk_complete();
    } else if (off >= 0x14 && off < 0x1C && is_in) {
        const struct windows_guest_info *info = windows_guest_get_info();
        u64 sectors = info ? info->esp_image_size / 512u : 0;
        *result = (sectors >> ((off - 0x14) * 8)) &
                  (size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF);
    }
    (void)size;
    return 1;
}

static u8 cmos_read(u8 reg) {
    switch (reg & 0x7F) {
    case 0x0A: return 0x26; /* divider + 1024 Hz periodic rate */
    case 0x0B: return 0x02; /* 24-hour, BCD */
    case 0x0C: return 0;
    case 0x0D: return 0x80;
    case 0x15: return 0x80; /* 640 KiB base memory */
    case 0x16: return 0x02;
    case 0x17: return 0x00; /* 63 MiB extended memory in KiB */
    case 0x18: return 0xFC;
    case 0x30: return 0x00;
    case 0x31: return 0xFC;
    case 0x34: return 0x00; /* memory above 16 MiB in 64 KiB units */
    case 0x35: return 0x3F; /* 4 GiB visible below PCI hole */
    case 0x5B: return 0x00;
    case 0x5C: return 0x00;
    case 0x5D: return 0x10; /* 1 GiB above 4 GiB */
    default: return 0;
    }
}

static u8 fw_cfg_byte(u16 selector, u32 offset) {
    static const u8 signature[4] = { 'Q', 'E', 'M', 'U' };
    static const u8 id[4] = { 1, 0, 0, 0 }; /* FW_CFG_VERSION, no DMA */
    /* OFW path of the legacy virtio-blk at slot 3 function 0. */
    static const u8 bootorder[] =
        "/pci@i0cf8/scsi@3/disk@0,0\n";
    static const u8 shell[] = "no";
    static const u8 dir[] = {
0x00,
        0x00,
        0x00,
        0x02,
        0x00,
        0x00,
        0x00,
        0x1b,
        0x00,
        0x20,
        0x00,
        0x00,
        0x62,
        0x6f,
        0x6f,
        0x74,
        0x6f,
        0x72,
        0x64,
        0x65,
        0x72,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x02,
        0x00,
        0x21,
        0x00,
        0x00,
        0x6f,
        0x70,
        0x74,
        0x2f,
        0x6f,
        0x72,
        0x67,
        0x2e,
        0x74,
        0x69,
        0x61,
        0x6e,
        0x6f,
        0x63,
        0x6f,
        0x72,
        0x65,
        0x2f,
        0x45,
        0x46,
        0x49,
        0x53,
        0x68,
        0x65,
        0x6c,
        0x6c,
        0x53,
        0x75,
        0x70,
        0x70,
        0x6f,
        0x72,
        0x74,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00
    };
    if (selector == 0x0000 && offset < sizeof(signature))
        return signature[offset];
    if (selector == 0x0001 && offset < sizeof(id))
        return id[offset];
    if (selector == 0x000E && offset == 0)
        return 0; /* boot menu disabled */
    if (selector == 0x000F && offset < 4)
        return 0; /* maximum CPUs is supplied through ACPI later */
    if (selector == 0x0019 && offset < sizeof(dir))
        return dir[offset];
    if (selector == 0x0020 && offset < sizeof(bootorder) - 1)
        return bootorder[offset];
    if (selector == 0x0021 && offset < sizeof(shell) - 1)
        return shell[offset];
    return 0;
}

int windows_platform_io(u16 port, int is_in, u32 size, u64 value,
                        u64 *result, int *terminate) {
    if (!g_pc.initialized || !result || !terminate) return 0;
    *result = 0;
    *terminate = 0;
    if (virtio_blk_io(port, is_in, size, value, result)) return 1;

    if (port == 0xCF8 && size == 4) {
        if (is_in) *result = g_pc.pci_addr;
        else g_pc.pci_addr = (u32)value;
        return 1;
    }
    if (port >= 0xCFC && port <= 0xCFF) {
        if (!(g_pc.pci_addr & PCI_CFG_ENABLE)) {
            *result = 0xFFFFFFFFu;
            return 1;
        }
        u8 bus = (u8)((g_pc.pci_addr >> 16) & 0xFF);
        u8 dev = (u8)((g_pc.pci_addr >> 11) & 0x1F);
        u8 fn = (u8)((g_pc.pci_addr >> 8) & 0x07);
        u8 reg = (u8)(g_pc.pci_addr & 0xFC);
        u32 data = pci_config_dword(bus, dev, fn, reg);
        u32 shift = (u32)(port - 0xCFC) * 8;
        if (is_in) {
            if (size == 1) *result = (data >> shift) & 0xFF;
            else if (size == 2) *result = (data >> shift) & 0xFFFF;
            else *result = data;
        } else {
            u32 mask = size == 1 ? 0xFFu << shift :
                       size == 2 ? 0xFFFFu << shift : 0xFFFFFFFFu;
            pci_config_write(bus, dev, fn, reg,
                             (data & ~mask) | (((u32)value << shift) & mask));
        }
        return 1;
    }

    if (port == 0x510) {
        if (is_in) {
            *result = g_pc.fw_cfg_selector;
        } else {
            g_pc.fw_cfg_selector = (u16)value;
            g_pc.fw_cfg_offset = 0;
            static u32 fw_logs;
            if (fw_logs < 16 || value == 0x21) {
                fw_logs++;
                log_hex64("[WIN] fwcfg sel=", value);
            }
        }
        return 1;
    }
    if (port == 0x511) {
        if (is_in) {
            u64 out = 0;
            for (u32 i = 0; i < size; i++) {
                out |= (u64)fw_cfg_byte(g_pc.fw_cfg_selector,
                                       g_pc.fw_cfg_offset++) << (i * 8);
            }
            *result = out;
        }
        return 1;
    }

    if (port == 0x70) {
        if (is_in) *result = g_pc.cmos_index;
        else g_pc.cmos_index = (u8)value;
        return 1;
    }
    if (port == 0x71) {
        if (is_in) *result = cmos_read(g_pc.cmos_index);
        return 1;
    }

    if (port >= 0x600 && port < 0x610) {
        u32 index = (u32)(port - 0x600);
        if (port >= 0x608 && port + size <= 0x60C) {
            u32 timer = (u32)(rdtsc_local() / 30u) & 0x00FFFFFFu;
            if (is_in) *result = timer >> ((port - 0x608) * 8);
            return 1;
        }
        if (is_in) {
            u32 data = 0;
            for (u32 i = 0; i < size && index + i < 16; i++)
                data |= (u32)g_pc.pm_io[index + i] << (i * 8);
            *result = data;
        } else {
            if (port == 0x604 && (value & (1u << 13)) &&
                ((value >> 10) & 7) == 5)
                *terminate = 1;
            for (u32 i = 0; i < size && index + i < 16; i++)
                g_pc.pm_io[index + i] = (u8)(value >> (i * 8));
        }
        return 1;
    }
    if (port == 0x06 && is_in) {
        /* Firmware polls this dword until it changes. A stuck zero
         * livelocks SEC after the LPC PMBASE probe. */
        static int logged;
        if (!logged) {
            logged = 1;
            log_hex64("[WIN] port6 counter rip=", 0);
        }
        *result = (u32)(rdtsc_local() >> 4);
        return 1;
    }
    if (port == 0x92) {
        if (is_in) *result = 0x02; /* A20 enabled */
        else if (value & 1) *terminate = 1;
        return 1;
    }
    if (port == 0xCF9) {
        if (!is_in && (value & 0x06)) *terminate = 1;
        return 1;
    }
    if (port == 0x80 && !is_in) {
        static u8 last = 0xFF;
        static u32 count;
        u8 code = (u8)value;
        if (code != last && count < 96) {
            last = code;
            count++;
            log_hex64("[WIN] POST=", code);
        }
        return 1;
    }
    if (port == 0x402) {
        if (!is_in) {
            char c = (char)value;
            if (c >= 32 || c == '\n' || c == '\r') {
                extern void serial_putc(char c);
                serial_putc(c);
            }
        }
        return 1;
    }
    return 0;
}

int windows_platform_string_io(u16 port, int is_in, u32 size, u64 count,
                               u64 linear, int down) {
    if (size == 0 || size > 4 || count > 65536) return -1;
    for (u64 n = 0; n < count; n++) {
        u64 gpa = windows_gva_to_gpa(linear);
        if (!gpa) return -1;
        u64 value = 0;
        int terminate = 0;
        if (!is_in) {
            u8 raw[4];
            if (gpa_copy(gpa, raw, size, 0) != 0) return -1;
            for (u32 i = 0; i < size; i++) value |= (u64)raw[i] << (i * 8);
        }
        u64 result = 0;
        if (!windows_platform_io(port, is_in, size, value, &result, &terminate))
            result = is_in ? ~0ULL : 0;
        if (is_in) {
            u8 raw[4];
            for (u32 i = 0; i < size; i++) raw[i] = (u8)(result >> (i * 8));
            if (gpa_copy(gpa, raw, size, 1) != 0) return -1;
        }
        if (terminate) return -1;
        linear += down ? -(u64)size : (u64)size;
    }
    return 0;
}

int windows_platform_is_mmio(u64 gpa) {
    return (gpa >= WINDOWS_PCIE_ECAM_BASE &&
            gpa < WINDOWS_PCIE_ECAM_BASE + WINDOWS_PCIE_ECAM_SIZE) ||
           (gpa >= WINDOWS_IOAPIC_BASE && gpa < WINDOWS_IOAPIC_BASE + 0x1000) ||
           (gpa >= WINDOWS_HPET_BASE && gpa < WINDOWS_HPET_BASE + 0x1000) ||
           (gpa >= WINDOWS_LAPIC_BASE && gpa < WINDOWS_LAPIC_BASE + 0x1000);
}

static u32 ioapic_read(u32 reg) {
    if (reg == 0x00) return g_pc.ioapic_id << 24;
    if (reg == 0x01) return 0x00170020u; /* version 0x20, 24 entries */
    if (reg == 0x02) return g_pc.ioapic_id << 24;
    if (reg >= 0x10 && reg < 0x40) {
        u32 index = (reg - 0x10) / 2;
        return (reg & 1) ? (u32)(g_pc.ioapic_redir[index] >> 32)
                         : (u32)g_pc.ioapic_redir[index];
    }
    return 0;
}

static void ioapic_write(u32 reg, u32 value) {
    if (reg == 0x00) {
        g_pc.ioapic_id = (value >> 24) & 0x0F;
    } else if (reg >= 0x10 && reg < 0x40) {
        u32 index = (reg - 0x10) / 2;
        if (reg & 1)
            g_pc.ioapic_redir[index] =
                (g_pc.ioapic_redir[index] & 0xFFFFFFFFULL) |
                ((u64)value << 32);
        else
            g_pc.ioapic_redir[index] =
                (g_pc.ioapic_redir[index] & 0xFFFFFFFF00000000ULL) | value;
    }
}

static u32 active_apic_id(void) {
    struct vm_vcpu *vcpu = vm_vcpu_active();
    return vcpu ? vcpu->apic_id : 0;
}

u64 windows_platform_apic_base_read(void) {
    u32 id = active_apic_id();
    if (id >= WINDOWS_VCPU_COUNT) id = 0;
    return g_pc.lapic_base[id];
}

void windows_platform_apic_base_write(u64 value) {
    u32 id = active_apic_id();
    if (id >= WINDOWS_VCPU_COUNT) id = 0;
    /* Bits 8 (BSP), 10 (x2APIC) and 11 (enable), plus the base. */
    value &= 0x000FFFFFFFFFF000ULL | 0xD00ULL;
    if ((value & ~0xFFFULL) == 0)
        value |= 0xFEE00000ULL;
    /* The BSP flag is owned by the CPU. Clearing it makes OVMF park
     * the bootstrap processor in the AP halt loop. */
    if (id == 0) value |= 0x100ULL;
    else value &= ~0x100ULL;
    g_pc.lapic_base[id] = value;
}

u64 windows_platform_x2apic_read(u32 msr) {
    if (msr == 0x830) {
        u64 lo = windows_platform_mmio_read(WINDOWS_LAPIC_BASE + 0x300, 4);
        u64 hi = windows_platform_mmio_read(WINDOWS_LAPIC_BASE + 0x310, 4);
        return lo | (hi << 32);
    }
    return windows_platform_mmio_read(WINDOWS_LAPIC_BASE +
                                      ((u64)(msr - 0x800) << 4), 4);
}

void windows_platform_x2apic_write(u32 msr, u64 value) {
    if (msr == 0x830) {
        windows_platform_mmio_write(WINDOWS_LAPIC_BASE + 0x310, value >> 32, 4);
        windows_platform_mmio_write(WINDOWS_LAPIC_BASE + 0x300,
                                    value & 0xFFFFFFFFULL, 4);
        return;
    }
    windows_platform_mmio_write(WINDOWS_LAPIC_BASE +
                                ((u64)(msr - 0x800) << 4), value, 4);
}

int windows_platform_irq_allowed(u8 vector) {
    u32 id = active_apic_id();
    if (id >= WINDOWS_VCPU_COUNT) id = 0;
    return (vector >> 4) > (g_pc.lapic_tpr[id] >> 4);
}

void windows_platform_irq_accepted(u8 vector) {
    u32 id = active_apic_id();
    if (id >= WINDOWS_VCPU_COUNT) id = 0;
    g_pc.lapic_isr[id][vector >> 5] |= 1u << (vector & 31);
}

static u64 hpet_counter(void) {
    if (!(g_pc.hpet_config & 1)) return g_pc.hpet_counter_base;
    u64 delta = rdtsc_local() - g_pc.hpet_tsc_base;
    return g_pc.hpet_counter_base + delta / 20; /* nominal 10 MHz */
}

u64 windows_platform_mmio_read(u64 gpa, u32 width) {
    (void)width;
    if (gpa >= WINDOWS_PCIE_ECAM_BASE &&
        gpa < WINDOWS_PCIE_ECAM_BASE + WINDOWS_PCIE_ECAM_SIZE) {
        u64 off = gpa - WINDOWS_PCIE_ECAM_BASE;
        u8 bus = (u8)((off >> 20) & 0xFF);
        u8 dev = (u8)((off >> 15) & 0x1F);
        u8 fn = (u8)((off >> 12) & 0x07);
        u8 reg = (u8)(off & 0xFC);
        u32 data = pci_config_dword(bus, dev, fn, reg);
        return data >> ((off & 3) * 8);
    }
    if (gpa >= WINDOWS_IOAPIC_BASE && gpa < WINDOWS_IOAPIC_BASE + 0x1000) {
        u32 off = (u32)(gpa - WINDOWS_IOAPIC_BASE);
        if (off == 0x00) return g_pc.ioapic_select;
        if (off == 0x10) return ioapic_read(g_pc.ioapic_select);
        return 0;
    }
    if (gpa >= WINDOWS_LAPIC_BASE && gpa < WINDOWS_LAPIC_BASE + 0x1000) {
        u32 off = (u32)(gpa - WINDOWS_LAPIC_BASE);
        u32 id = active_apic_id();
        if (id >= WINDOWS_VCPU_COUNT) id = 0;
        if (off == 0x20) return id << 24;
        if (off == 0x30) return 0x00060014u;
        if (off == 0x80) return g_pc.lapic_tpr[id];
        if (off >= 0x100 && off < 0x180 && (off & 0xF) == 0)
            return g_pc.lapic_isr[id][(off - 0x100) >> 4];
        if (off == 0xF0) return g_pc.lapic_svr[id];
        if (off == 0x320) return g_pc.lapic_lvt_timer[id];
        if (off == 0x310) return g_pc.lapic_icr_high[id];
        if (off == 0x380) return g_pc.lapic_initial_count[id];
        if (off == 0x390) return g_pc.lapic_current_count[id];
        if (off == 0x3E0) return g_pc.lapic_divide[id];
        return 0;
    }
    if (gpa >= WINDOWS_HPET_BASE && gpa < WINDOWS_HPET_BASE + 0x1000) {
        u32 off = (u32)(gpa - WINDOWS_HPET_BASE);
        if (off == 0x000) return 0x8086A201ULL | (100000000ULL << 32);
        if (off == 0x010) return g_pc.hpet_config;
        if (off == 0x0F0) return hpet_counter();
        return 0;
    }
    return 0;
}

void windows_platform_mmio_write(u64 gpa, u64 value, u32 width) {
    if (gpa >= WINDOWS_PCIE_ECAM_BASE &&
        gpa < WINDOWS_PCIE_ECAM_BASE + WINDOWS_PCIE_ECAM_SIZE) {
        u64 off = gpa - WINDOWS_PCIE_ECAM_BASE;
        u8 bus = (u8)((off >> 20) & 0xFF);
        u8 dev = (u8)((off >> 15) & 0x1F);
        u8 fn = (u8)((off >> 12) & 0x07);
        u8 reg = (u8)(off & 0xFC);
        u32 shift = (u32)(off & 3) * 8;
        u32 mask = width == 1 ? 0xFFu << shift :
                   width == 2 ? 0xFFFFu << shift : 0xFFFFFFFFu;
        u32 data = pci_config_dword(bus, dev, fn, reg);
        pci_config_write(bus, dev, fn, reg,
                         (data & ~mask) | (((u32)value << shift) & mask));
        return;
    }
    (void)width;
    if (gpa >= WINDOWS_IOAPIC_BASE && gpa < WINDOWS_IOAPIC_BASE + 0x1000) {
        u32 off = (u32)(gpa - WINDOWS_IOAPIC_BASE);
        if (off == 0x00) g_pc.ioapic_select = (u32)value;
        else if (off == 0x10) ioapic_write(g_pc.ioapic_select, (u32)value);
        return;
    }
    if (gpa >= WINDOWS_LAPIC_BASE && gpa < WINDOWS_LAPIC_BASE + 0x1000) {
        u32 off = (u32)(gpa - WINDOWS_LAPIC_BASE);
        u32 id = active_apic_id();
        if (id >= WINDOWS_VCPU_COUNT) id = 0;
        if (off == 0x80) g_pc.lapic_tpr[id] = (u32)value & 0xFF;
        else if (off == 0xB0) {
            static u32 eoi_logs;
            if (eoi_logs < 4) {
                eoi_logs++;
                log_hex64("[WIN] lapic eoi=", vmx_vmcs_read(VMCS_GUEST_RIP));
            }
            for (int reg = 7; reg >= 0; reg--) {
                u32 bits = g_pc.lapic_isr[id][reg];
                if (!bits) continue;
                u32 bit = 31;
                while (bit && !(bits & (1u << bit))) bit--;
                g_pc.lapic_isr[id][reg] = bits & ~(1u << bit);
                break;
            }
        }
        else if (off == 0xF0) g_pc.lapic_svr[id] = (u32)value;
        else if (off == 0x310) g_pc.lapic_icr_high[id] = (u32)value;
        else if (off == 0x300) {
            static u32 icr_logs;
            if (icr_logs < 8) {
                icr_logs++;
                log_hex64("[WIN] icr low=", value);
                log_hex64("[WIN] icr high=", g_pc.lapic_icr_high[id]);
            }
            u32 delivery = ((u32)value >> 8) & 7;
            u32 shorthand = ((u32)value >> 18) & 3;
            u32 destination = g_pc.lapic_icr_high[id] >> 24;
            if (delivery == 5 && (shorthand == 3 ||
                                  (shorthand == 0 && destination == 1)))
                windows_vcpu_reset_ap();
            if (delivery == 6 && (shorthand == 3 ||
                                  (shorthand == 0 && destination == 1))) {
                if (windows_vcpu_start_ap((u8)value) != 0) {
                    panic_full("WIN-E34 SIPI DELIVERY FAILED",
                               "could not start Windows AP from LAPIC SIPI", 0);
                }
            }
        }
        else if (off == 0x320) g_pc.lapic_lvt_timer[id] = (u32)value;
        else if (off == 0x380) {
            g_pc.lapic_initial_count[id] = (u32)value;
            g_pc.lapic_current_count[id] = (u32)value;
            g_pc.lapic_last_tsc[id] = rdtsc_local();
        }
        else if (off == 0x3E0) g_pc.lapic_divide[id] = (u32)value;
        return;
    }
    if (gpa >= WINDOWS_HPET_BASE && gpa < WINDOWS_HPET_BASE + 0x1000) {
        u32 off = (u32)(gpa - WINDOWS_HPET_BASE);
        if (off == 0x010) {
            g_pc.hpet_counter_base = hpet_counter();
            g_pc.hpet_tsc_base = rdtsc_local();
            g_pc.hpet_config = value & 3;
        } else if (off == 0x0F0) {
            g_pc.hpet_counter_base = value;
            g_pc.hpet_tsc_base = rdtsc_local();
        }
    }
}

int windows_platform_irq_armed(void) {
    return g_pc.irq_armed;
}

void windows_platform_arm_irq(void) {
    if (!g_pc.irq_armed) {
        vmx_guest_irq_reset();
        g_pc.irq_armed = 1;
        log_info("[WIN] guest idle with interrupts; timer delivery armed");
        windows_log_sleep_caller();
    }
}

static u64 windows_gva_to_gpa(u64 gva) {
    u64 cr0 = vmx_vmcs_read(VMCS_GUEST_CR0);
    u64 cr3 = vmx_vmcs_read(VMCS_GUEST_CR3);
    u64 cr4 = vmx_vmcs_read(VMCS_GUEST_CR4);
    u64 efer = vmx_vmcs_read(VMCS_GUEST_IA32_EFER);
    u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
    if (!(cr0 & CR0_PG)) return gva;
    if (!hhdm) return 0;
    if (!(efer & EFER_LMA)) {
        if (!(cr4 & CR4_PAE)) {
            u64 table = cr3 & ~0xFFFULL;
            u64 hpa = ept_gpa_to_hpa(table);
            if (!hpa) return 0;
            u32 pde = ((u32 *)(hhdm + hpa))[(gva >> 22) & 0x3FF];
            if (!(pde & 1)) return 0;
            if (pde & (1u << 7))
                return (pde & 0xFFC00000u) + (gva & 0x3FFFFFULL);
            table = pde & ~0xFFFULL;
            hpa = ept_gpa_to_hpa(table);
            if (!hpa) return 0;
            u32 pte = ((u32 *)(hhdm + hpa))[(gva >> 12) & 0x3FF];
            if (!(pte & 1)) return 0;
            return (pte & ~0xFFFULL) + (gva & 0xFFFULL);
        }
        u64 pdpt_gpa = cr3 & ~0x1FULL;
        u64 pdpt_hpa = ept_gpa_to_hpa(pdpt_gpa & ~0xFFFULL);
        if (!pdpt_hpa) return 0;
        u64 pdpt = *(u64 *)(hhdm + pdpt_hpa + (pdpt_gpa & 0xFFF) +
                            ((gva >> 30) & 3) * 8);
        if (!(pdpt & 1)) return 0;
        if (pdpt & (1ULL << 7))
            return (pdpt & 0x000FFFFFC0000000ULL) + (gva & 0x3FFFFFFFULL);
        u64 table = pdpt & ~0xFFFULL;
        int shifts[2] = { 21, 12 };
        for (int level = 0; level < 2; level++) {
            u64 hpa = ept_gpa_to_hpa(table);
            if (!hpa) return 0;
            u64 entry = ((u64 *)(hhdm + hpa))[(gva >> shifts[level]) & 0x1FF];
            if (!(entry & 1)) return 0;
            if (level == 0 && (entry & (1ULL << 7)))
                return (entry & 0x000FFFFFFFE00000ULL) + (gva & 0x1FFFFFULL);
            table = entry & ~0xFFFULL;
        }
        return table + (gva & 0xFFFULL);
    }
    u64 table = cr3 & ~0xFFFULL;
    int shifts[4] = { 39, 30, 21, 12 };
    for (int level = 0; level < 4; level++) {
        u64 hpa = ept_gpa_to_hpa(table);
        if (!hpa) return 0;
        u64 entry = ((u64 *)(hhdm + hpa))[(gva >> shifts[level]) & 0x1FF];
        if (!(entry & 1)) return 0;
        if ((level == 1 || level == 2) && (entry & (1ULL << 7))) {
            u64 mask = level == 1 ? 0x3FFFFFFFULL : 0x1FFFFFULL;
            u64 base = level == 1 ? (entry & 0x000FFFFFC0000000ULL)
                                  : (entry & 0x000FFFFFFFE00000ULL);
            return base + (gva & mask);
        }
        table = entry & ~0xFFFULL;
    }
    return table + (gva & 0xFFFULL);
}

static void windows_log_sleep_caller(void) {
    u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
    u64 rsp = vmx_vmcs_read(VMCS_GUEST_RSP);
    u64 rsp_gpa = windows_gva_to_gpa(rsp);
    u64 rsp_hpa = rsp_gpa ? ept_gpa_to_hpa(rsp_gpa) : 0;
    log_hex64("[WIN] sleep rsp=", rsp);
    log_hex64("[WIN] sleep idtr=", vmx_vmcs_read(VMCS_GUEST_IDTR_BASE));
    log_hex64("[WIN] sleep lvt=", g_pc.lapic_lvt_timer[active_apic_id()]);
    log_hex64("[WIN] sleep hpet=", g_pc.hpet_config);
    {
        u64 idtr = vmx_vmcs_read(VMCS_GUEST_IDTR_BASE);
        u64 gate_gpa = windows_gva_to_gpa(idtr + 0x20u * 16u);
        u64 gate_hpa = gate_gpa ? ept_gpa_to_hpa(gate_gpa) : 0;
        if (hhdm && gate_hpa && (gate_gpa & 0xFFF) <= 0xFF0) {
            u8 *g = (u8 *)(hhdm + gate_hpa);
            u64 off = (u64)g[0] | ((u64)g[1] << 8) |
                      ((u64)g[6] << 16) | ((u64)g[7] << 24) |
                      ((u64)*(u32 *)(g + 8) << 32);
            log_hex64("[WIN] sleep timer isr=", off);
            log_hex64("[WIN] sleep gate type=", g[5]);
            u64 isr_gpa = windows_gva_to_gpa(off);
            u64 isr_hpa = isr_gpa ? ept_gpa_to_hpa(isr_gpa) : 0;
            if (isr_hpa && (isr_gpa & 0xFFF) <= 0xFF0) {
                u8 *ip = (u8 *)(hhdm + isr_hpa);
                u64 word = 0;
                for (int i = 0; i < 8; i++) word |= (u64)ip[i] << (i * 8);
                log_hex64("[WIN] sleep isr bytes=", word);
                /* CpuDxe's vector table sits a fixed distance after the
                 * vector-0x20 stub in this firmware build. */
                u64 slot_gpa = windows_gva_to_gpa(off + 0x26D0);
                u64 slot_hpa = slot_gpa ? ept_gpa_to_hpa(slot_gpa) : 0;
                if (slot_hpa && (slot_gpa & 0xFFF) <= 0xFF8) {
                    u64 table = *(u64 *)(hhdm + slot_hpa);
                    log_hex64("[WIN] sleep irq table=", table);
                    u64 ent = table + 0x20u * 0x60u;
                    u64 ent_gpa = windows_gva_to_gpa(ent);
                    u64 ent_hpa = ent_gpa ? ept_gpa_to_hpa(ent_gpa) : 0;
                    if (ent_hpa && (ent_gpa & 0xFFF) <= 0xFE0) {
                        u8 *e = (u8 *)(hhdm + ent_hpa);
                        u32 attr = *(u32 *)(e + 0x0C);
                        u64 fn = *(u64 *)(e + 0x10);
                        log_hex64("[WIN] sleep irq attr=", attr);
                        log_hex64("[WIN] sleep irq fn=", fn);
                    }
                    for (u32 vec = 0; vec < 256; vec++) {
                        u64 ent = table + (u64)vec * 0x60u;
                        u64 eg = windows_gva_to_gpa(ent + 0x0C);
                        u64 eh = eg ? ept_gpa_to_hpa(eg) : 0;
                        if (!eh || (eg & 0xFFF) > 0xFFC) continue;
                        u32 attr = *(u32 *)(hhdm + eh);
                        if (attr == 0xFFFFFFFF) continue;
                        log_hex64("[WIN] sleep vec=", vec);
                        log_hex64("[WIN] sleep vec attr=", attr);
                    }
                    log_hex64("[WIN] sleep ioapic0=", g_pc.ioapic_redir[0]);
                    log_hex64("[WIN] sleep ioapic2=", g_pc.ioapic_redir[2]);
                    /* The next qword is ExternalInterruptHandler[]. */
                    u64 ext_gpa = windows_gva_to_gpa(off + 0x26D8);
                    u64 ext_hpa = ext_gpa ? ept_gpa_to_hpa(ext_gpa) : 0;
                    if (ext_hpa && (ext_gpa & 0xFFF) <= 0xFF8) {
                        u64 handlers = *(u64 *)(hhdm + ext_hpa);
                        log_hex64("[WIN] sleep ext table=", handlers);
                        u64 slot = handlers + 0x20u * 8u;
                        u64 sg = windows_gva_to_gpa(slot);
                        u64 sh = sg ? ept_gpa_to_hpa(sg) : 0;
                        if (sh && (sg & 0xFFF) <= 0xFF8)
                            log_hex64("[WIN] sleep ext32=",
                                      *(u64 *)(hhdm + sh));
                    }
                }
            }
        }
    }
    if (!rsp_hpa || !hhdm) return;
    u64 ret = *(u64 *)(hhdm + rsp_hpa);
    log_hex64("[WIN] sleep ret=", ret);
    for (int back = 64; back >= 0; back -= 8) {
        u64 gpa = windows_gva_to_gpa(ret - (u64)back);
        u64 hpa = gpa ? ept_gpa_to_hpa(gpa) : 0;
        if (!hpa || (gpa & 0xFFF) > 0xFF8) continue;
        u8 *p = (u8 *)(hhdm + hpa);
        u64 word = 0;
        for (int i = 0; i < 8; i++) word |= (u64)p[i] << (i * 8);
        log_hex64("[WIN] sleep code=", word);
    }
    u64 rbp = g_guest_regs.rbp;
    u64 outer = 0;
    for (int frame = 0; frame < 4; frame++) {
        u64 fp_gpa = windows_gva_to_gpa(rbp);
        u64 fp_hpa = fp_gpa ? ept_gpa_to_hpa(fp_gpa) : 0;
        if (!fp_hpa || (fp_gpa & 0xFFF) > 0xFF0) break;
        u64 next = *(u64 *)(hhdm + fp_hpa);
        u64 site = *(u64 *)(hhdm + fp_hpa + 8);
        log_hex64("[WIN] sleep frame=", site);
        outer = site;
        if (!next || next == rbp) break;
        rbp = next;
    }
    if (outer) {
        for (u64 gva = outer & ~0xFFFULL; gva > outer - 0x400000; gva -= 0x1000) {
            u64 gpa = windows_gva_to_gpa(gva);
            u64 hpa = gpa ? ept_gpa_to_hpa(gpa) : 0;
            if (!hpa) continue;
            u8 *page = (u8 *)(hhdm + hpa);
            if (page[0] != 'M' || page[1] != 'Z') continue;
            log_hex64("[WIN] sleep caller image=", gva);
            {
                u32 lfanew = *(u32 *)(page + 0x3C);
                if (lfanew >= 0x40 && lfanew <= 0x200) {
                    u64 pe_gpa = windows_gva_to_gpa(gva + lfanew);
                    u64 pe_hpa = pe_gpa ? ept_gpa_to_hpa(pe_gpa) : 0;
                    if (pe_hpa && (pe_gpa & 0xFFF) < 0xF00 &&
                        *(u32 *)(hhdm + pe_hpa) == 0x00004550) {
                        u8 *pe = (u8 *)(hhdm + pe_hpa);
                        u32 entry = *(u32 *)(pe + 24 + 16);
                        u32 imgsz = *(u32 *)(pe + 24 + 56);
                        log_hex64("[WIN] sleep caller entry=", entry);
                        log_hex64("[WIN] sleep caller size=", imgsz);
                        u64 eg = windows_gva_to_gpa(gva + entry);
                        u64 eh = eg ? ept_gpa_to_hpa(eg) : 0;
                        if (eh && (eg & 0xFFF) <= 0xFF0) {
                            u8 *ep = (u8 *)(hhdm + eh);
                            u64 word = 0;
                            for (int i = 0; i < 8; i++)
                                word |= (u64)ep[i] << (i * 8);
                            log_hex64("[WIN] sleep caller entryb=", word);
                        }
                    }
                }
            }
            break;
        }
    }
    log_hex64("[WIN] sleep r14=", g_guest_regs.r14);
    {
        u64 slot = g_guest_regs.r14 + 0xB8;
        u64 sg = windows_gva_to_gpa(slot);
        u64 sh = sg ? ept_gpa_to_hpa(sg) : 0;
        if (sh && (sg & 0xFFF) <= 0xFE0)
            log_hex64("[WIN] sleep evnt=", *(u64 *)(hhdm + sh));
    }
    for (u64 gva = ret & ~0xFFFULL; gva > ret - 0x200000; gva -= 0x1000) {
        u64 gpa = windows_gva_to_gpa(gva);
        u64 hpa = gpa ? ept_gpa_to_hpa(gpa) : 0;
        if (!hpa) continue;
        u8 *page = (u8 *)(hhdm + hpa);
        if (page[0] != 'M' || page[1] != 'Z') continue;
        log_hex64("[WIN] sleep image=", gva);
        u32 lfanew = *(u32 *)(page + 0x3C);
        if (lfanew < 0x40 || lfanew > 0x400) break;
        u64 pe_gpa = windows_gva_to_gpa(gva + lfanew);
        u64 pe_hpa = pe_gpa ? ept_gpa_to_hpa(pe_gpa) : 0;
        if (!pe_hpa || (pe_gpa & 0xFFF) > 0xF00) break;
        u8 *pe = (u8 *)(hhdm + pe_hpa);
        if (*(u32 *)pe != 0x00004550) break;
        u16 magic = *(u16 *)(pe + 24);
        u32 dir_off = magic == 0x20B ? 24 + 112 + 6 * 8 : 24 + 96 + 6 * 8;
        u32 dbg_rva = *(u32 *)(pe + dir_off);
        u32 dbg_sz = *(u32 *)(pe + dir_off + 4);
        log_hex64("[WIN] sleep dbg rva=", dbg_rva);
        if (!dbg_rva || dbg_sz < 28) break;
        u64 cv_gpa = windows_gva_to_gpa(gva + dbg_rva + 20);
        u64 cv_hpa = cv_gpa ? ept_gpa_to_hpa(cv_gpa) : 0;
        if (!cv_hpa) break;
        u32 cv_rva = *(u32 *)(hhdm + cv_hpa);
        u64 name_gpa = windows_gva_to_gpa(gva + cv_rva + 24);
        u64 name_hpa = name_gpa ? ept_gpa_to_hpa(name_gpa) : 0;
        if (!name_hpa || (name_gpa & 0xFFF) > 0xFE0) break;
        u8 *name = (u8 *)(hhdm + name_hpa);
        u64 n0 = 0, n1 = 0;
        for (int i = 0; i < 8; i++) {
            n0 |= (u64)name[i] << (i * 8);
            n1 |= (u64)name[8 + i] << (i * 8);
        }
        log_hex64("[WIN] sleep pdb0=", n0);
        log_hex64("[WIN] sleep pdb1=", n1);
        break;
    }
    u64 prev_gpa = windows_gva_to_gpa(ret - 16);
    u64 prev_hpa = prev_gpa ? ept_gpa_to_hpa(prev_gpa) : 0;
    if (prev_hpa && (prev_gpa & 0xFFF) <= 0xFF0) {
        u8 *prev = (u8 *)(hhdm + prev_hpa);
        u64 word = 0;
        for (int i = 0; i < 8; i++) word |= (u64)prev[i] << (i * 8);
        log_hex64("[WIN] sleep before-16=", word);
        word = 0;
        for (int i = 0; i < 8; i++) word |= (u64)prev[8 + i] << (i * 8);
        log_hex64("[WIN] sleep before-8=", word);
    }
    u64 ret_gpa = windows_gva_to_gpa(ret);
    u64 ret_hpa = ret_gpa ? ept_gpa_to_hpa(ret_gpa) : 0;
    if (!ret_hpa || (ret_gpa & 0xFFF) > 0xFF0) return;
    u8 *code = (u8 *)(hhdm + ret_hpa);
    u64 word = 0;
    for (int i = 0; i < 8; i++) word |= (u64)code[i] << (i * 8);
    log_hex64("[WIN] sleep caller=", word);
    word = 0;
    for (int i = 0; i < 8; i++) word |= (u64)code[8 + i] << (i * 8);
    log_hex64("[WIN] sleep caller+8=", word);
}

void windows_platform_tick(void) {
    u32 id = active_apic_id();
    if (id >= WINDOWS_VCPU_COUNT || !g_pc.lapic_current_count[id]) return;
    u64 now = rdtsc_local();
    u64 delta = now - g_pc.lapic_last_tsc[id];
    g_pc.lapic_last_tsc[id] = now;
    u64 elapsed = delta / 20; /* expose a nominal 100 MHz LAPIC timer clock */
    if (elapsed < g_pc.lapic_current_count[id]) {
        g_pc.lapic_current_count[id] -= (u32)elapsed;
        return;
    }
    u32 lvt = g_pc.lapic_lvt_timer[id];
    g_pc.lapic_current_count[id] =
        (lvt & (1u << 17)) ? g_pc.lapic_initial_count[id] : 0;
    if (!g_pc.irq_armed) return;
    u8 vec = (u8)(lvt & 0xFF);
    if ((lvt & (1u << 16)) || vec < 0x20) vec = 0x20;
    u64 efer = vmx_vmcs_read(VMCS_GUEST_IA32_EFER);
    u64 cs = vmx_vmcs_read(VMCS_GUEST_CS_ACCESS);
    /* Compatibility mode keeps CS.L clear, but a long-mode IDT is 16 bytes. */
    u32 stride = (efer & EFER_LMA) ? 16u : 8u;
    u64 idtr = vmx_vmcs_read(VMCS_GUEST_IDTR_BASE);
    u64 idt_limit = vmx_vmcs_read(VMCS_GUEST_IDTR_LIMIT);
    static int logged;
    if ((u64)vec * stride + stride - 1 > idt_limit) {
        if (!logged) {
            logged = 1;
            log_hex64("[WIN] timer idt limit=", idt_limit);
        }
        return;
    }
    u64 gate_gpa = windows_gva_to_gpa(idtr + (u64)vec * stride);
    u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
    /* ept_gpa_to_hpa already includes the page offset. */
    if (!gate_gpa || !hhdm || (gate_gpa & 0xFFF) > 0x1000 - stride) {
        if (!logged) {
            logged = 1;
            log_hex64("[WIN] timer gate gpa=", gate_gpa);
            log_hex64("[WIN] timer cr3=", vmx_vmcs_read(VMCS_GUEST_CR3));
        }
        return;
    }
    u64 gate_hpa = ept_gpa_to_hpa(gate_gpa);
    if (!gate_hpa) {
        if (!logged) {
            logged = 1;
            log_hex64("[WIN] timer gate gpa=", gate_gpa);
            log_hex64("[WIN] timer cr3=", vmx_vmcs_read(VMCS_GUEST_CR3));
        }
        return;
    }
    u8 *gate = (u8 *)(hhdm + gate_hpa);
    if (gate[5] != 0x8E && gate[5] != 0x8F) {
        if (!logged) {
            logged = 1;
            u64 word = 0;
            for (u32 i = 0; i < 8 && i < stride; i++)
                word |= (u64)gate[i] << (i * 8);
            u64 zero_gpa = windows_gva_to_gpa(idtr);
            u8 zero_type = 0;
            if (zero_gpa) {
                u64 zh = ept_gpa_to_hpa(zero_gpa);
                if (zh)
                    zero_type = *(u8 *)(hhdm + zh + 5);
            }
            log_hex64("[WIN] timer gate type=", gate[5]);
            log_hex64("[WIN] timer gate bytes=", word);
            log_hex64("[WIN] timer gate gpa=", gate_gpa);
            log_hex64("[WIN] timer idt0 type=", zero_type);
            log_hex64("[WIN] timer cr3=", vmx_vmcs_read(VMCS_GUEST_CR3));
            log_hex64("[WIN] timer cr4=", vmx_vmcs_read(VMCS_GUEST_CR4));
            log_hex64("[WIN] timer efer=", efer);
            log_hex64("[WIN] timer idt limit=", idt_limit);
            {
                u64 cr3 = vmx_vmcs_read(VMCS_GUEST_CR3) & ~0xFFFULL;
                u64 cr3_hpa = ept_gpa_to_hpa(cr3);
                u64 pml4 = 0;
                if (cr3_hpa && hhdm)
                    pml4 = *(u64 *)(hhdm + cr3_hpa);
                log_hex64("[WIN] timer pml4[0]=", pml4);
                u64 found = 0;
                u32 hits = 0;
                u64 rsp = vmx_vmcs_read(VMCS_GUEST_RSP);
                u64 rsp_gpa = windows_gva_to_gpa(rsp);
                u64 rsp_hpa = rsp_gpa ? ept_gpa_to_hpa(rsp_gpa) : 0;
                log_hex64("[WIN] timer rsp=", rsp);
                if (rsp_hpa)
                    log_hex64("[WIN] timer ret=", *(u64 *)(hhdm + rsp_hpa));
                for (u64 gpa = 0x3E000000ULL;
                     gpa < 0x42000000ULL && hits < 4; gpa += 0x1000) {
                    u64 hp = ept_gpa_to_hpa(gpa);
                    if (!hp) continue;
                    u8 *page = (u8 *)(hhdm + hp);
                    u32 present = 0;
                    for (u32 slot = 0; slot < 32; slot++) {
                        u8 kind = page[slot * 16 + 5];
                        if (kind == 0x8E || kind == 0x8F) present++;
                    }
                    if (present >= 8) {
                        hits++;
                        if (!found) found = gpa;
                    }
                }
                log_hex64("[WIN] timer idt candidate=", found);
                log_hex64("[WIN] timer idt hits=", hits);
            }
            (void)cs;
        }
        return;
    }
    vmx_guest_queue_irq(vec);
}

int windows_platform_init(struct vm_machine *machine) {
    if (!machine || machine->profile != VM_PROFILE_WINDOWS ||
        machine->vcpu_count != WINDOWS_VCPU_COUNT) {
        panic_full("WIN-E17 INVALID WINDOWS PLATFORM CONTRACT",
                   "Windows platform requires a two-vcpu Windows machine", 0);
        return -1;
    }
    zero_bytes(&g_pc, sizeof(g_pc));
    g_pc.ioapic_id = 2;
    for (u32 i = 0; i < 24; i++)
        g_pc.ioapic_redir[i] = (1ULL << 16) | (0x20 + i);
    for (u32 i = 0; i < WINDOWS_VCPU_COUNT; i++) {
        g_pc.lapic_svr[i] = 0x1FF;
        g_pc.lapic_lvt_timer[i] = 1u << 16;
        g_pc.lapic_divide[i] = 3;
        g_pc.lapic_last_tsc[i] = rdtsc_local();
        /* EN, base 0xFEE00000. Only vCPU 0 is the BSP. */
        g_pc.lapic_base[i] = 0xFEE00800ULL | (i == 0 ? 0x100ULL : 0);
    }
    g_pc.hpet_tsc_base = rdtsc_local();
    g_pc.pci_mch[0] = 0x29C08086u;
    g_pc.pci_mch[1] = 0x00100006u;
    g_pc.pci_mch[2] = 0x06000002u;
    g_pc.pci_lpc[0] = 0x29188086u;
    g_pc.pci_lpc[1] = 0x00100007u;
    g_pc.pci_lpc[2] = 0x06010002u;
    g_pc.pci_lpc[3] = 0x00800000u;
    g_pc.pci_lpc[0x40 / 4] = 0x00000601u; /* PMBASE 0x600, IO space enabled */
    g_pc.pci_lpc[0x44 / 4] = 0x00000080u; /* ACPI_EN */
    {
        const struct windows_guest_info *info = windows_guest_get_info();
        log_hex64("[WIN] virtio-blk sectors=",
                  info ? info->esp_image_size / 512u : 0);
    }
    if (windows_acpi_build() != 0) return -2;
    g_pc.initialized = 1;
    log_info("[WIN] q35-like PCI/APIC/HPET platform ready");
    return 0;
}
