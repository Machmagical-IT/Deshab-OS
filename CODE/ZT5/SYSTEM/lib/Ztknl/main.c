#include <stddef.h>
#include "limine.h"
#include "console.h"
#include "string.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "cpu.h"
#include "apic.h"
#include "task.h"
#include "scheduler.h"
#include "vfs.h"
#include "fat32.h"
#include "disk.h"
#include "elf.h"
#include "syscall.h"
#include "config.h"
#include "abi.h"
#include "fb.h"
#include "image.h"
#include "net.h"
#include "socket.h"
#include "ext4.h"
#include "loader.h"
#include "subkernel.h"
#include "hotupdate.h"
#include "ipc.h"
#include "input.h"
#include "inter_kernel.h"
#include "dual_kernel.h"
#include "exec.h"
#include "fs_proxy.h"  /* 文件系统 RPC 代理 */
#include "tty.h"       /* TTY 子系统 */
#include "e1000.h"     /* Intel e1000 网卡驱动 */
#include "nvme.h"      /* NVMe 存储驱动 */
#include "zts_algorithm.h"  /* ZTS7 动态算法系统 */
#include "wifi.h"      /* WiFi 无线驱动 */
#include "ai.h"        /* AI 自开发子系统 */
#include "ps2.h"       /* PS/2 键盘/鼠标驱动 */

/* 全局变量：用户空间 framebuffer 虚拟地址（供内核刷新使用） */
uint64_t g_user_fb_virt = 0;

/* 全局变量：启动时加载的用户配置 */
uint32_t g_boot_cfg_uid = 1000;
uint32_t g_boot_cfg_gid = 1000;
char g_boot_cfg_username[32] = "user";

/* 串口调试输出函数 (在 console.c 中定义) */
extern void serial_init(void);
extern void serial_print(const char *str);
extern void serial_putchar(char c);
#include "pe.h"
#include "ntdll.h"
#include "limine_file.h"
#include "smp.h"
#include "utx.h"       /* 用户态事务 API */
#include "pmem.h"      /* 持久内存管理 */
#include "zpipe.h"     /* 零拷贝管道 */
#include "capability.h" /* 能力安全系统 */
#include "selfheal.h"   /* 自愈内核框架 */
#include "coroutine.h"  /* 协程原生调度支持 */
#include "heterog.h"     /* 异构计算调度 */
#include "opu.h"         /* 开放处理单元支持 */
#include "page_fault.h"  /* 页错误处理和预防 */
#include "net_zerocopy.h" /* 网络零拷贝 */
#include "realtime.h"    /* 硬实时调度 */
#include "perf_analysis.h" /* 统一性能分析 */
#include "ktest.h"       /* 内核自测试框架 */
#include "rlimit.h"      /* 资源限制系统 */
#include "stack_protection.h" /* 栈保护机制 */
#include "kselfcheck.h"    /* 内核自检机制 */
#include "test_fixes.h"    /* 修复验证测试 */
#include "boot_diag.h"     /* 启动诊断和自检 */

/* ========== Limine 请求 ========== */

/* 请求开始标记 - 放入 .limine_reqs_start 段（必须放在最前面）*/
__attribute__((section(".limine_reqs_start"), used))
static uint64_t limine_requests_start_marker[4] = { 0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf,
                                                     0x785c6ed015d3e316, 0x181e920a7852b9d9 };

/* Limine 基础修订版本 - 使用 revision 3 (最新版本) */
__attribute__((section(".limine_reqs"), used))
static uint64_t limine_base_revision[3] = { 0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, 3 };

/* 帧缓冲请求 - 放入 .limine_reqs 段 */
__attribute__((section(".limine_reqs"), used))
volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = NULL
};

/* 内存映射请求 - 放入 .limine_reqs 段 */
__attribute__((section(".limine_reqs"), used))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = NULL
};

/* HHDM 请求 - 放入 .limine_reqs 段 */
__attribute__((section(".limine_reqs"), used))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL
};

/* 命令行请求 - 放入 .limine_reqs 段 */
__attribute__((section(".limine_reqs"), used))
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST,
    .revision = 0,
    .response = NULL
};

/* 主内核角色: 0=未知, 1=ztknl主, 2=azknl主 */
int primary_role = 0;

/* 解析命令行参数 */
static void parse_cmdline(void) {
    if (cmdline_request.response && cmdline_request.response->cmdline) {
        char *cmdline = (char *)cmdline_request.response->cmdline;
        if (strstr(cmdline, "primary=ztknl")) {
            primary_role = 1;
            print("[BOOT] Primary role: Ztknl\n");
        } else if (strstr(cmdline, "primary=azknl")) {
            primary_role = 2;
            print("[BOOT] Primary role: Azknl\n");
        }
    }
}

/* 请求结束标记 - 放入 .limine_reqs_end 段 */
__attribute__((section(".limine_reqs_end"), used))
static uint64_t limine_requests_end_marker[2] = { 0xadc0e0531bb10d03, 0x9572709f31764c62 };

/* ========== APIC 定时器中断处理 ========== */

extern void scheduler_tick(struct interrupt_frame *frame);

static void apic_timer_handler(struct interrupt_frame *frame) {
    scheduler_tick(frame);
}

static void apic_spurious_handler(struct interrupt_frame *frame) {
    (void)frame;
}

static void apic_error_handler(struct interrupt_frame *frame) {
    (void)frame;
    
    /* 读取错误状态寄存器 */
    extern uint32_t apic_read(uint32_t reg);
    uint32_t esr = apic_read(0x280);  /* APIC_ESR */
    
    /* 只在串口输出错误信息 */
    serial_print("[APIC] Error interrupt! ESR: 0x");
    char hex[11];
    hex[0] = '0'; hex[1] = 'x';
    for (int i = 0; i < 8; i++) {
        hex[2 + i] = "0123456789ABCDEF"[(esr >> (28 - i * 4)) & 0xF];
    }
    hex[10] = '\0';
    serial_print(hex);
    serial_print("\n");
    
    /* 清除错误状态 (必须先写后读) */
    extern void apic_write(uint32_t reg, uint32_t val);
    apic_write(0x280, 0);
    (void)apic_read(0x280);
    
    /* 发送 EOI */
    apic_eoi();
}

/* ========== 配置文件默认路径 ========== */

#define CONFIG_FILE_PATH    "/system/zratro.conf"

/* ========== 读取 TSC (正确方式) ========== */
static inline uint64_t rdtsc_safe(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ========== 启动程序 ========== */

/* 从 Limine 模块加载 ELF 程序 */
static int load_elf_from_limine(const char *module_name) {
    extern void *limine_find_module(const char *path, size_t *size_out);
    extern int elf_load_from_memory_ex(void *data, size_t size, const char *prog_name, struct process **proc_out, uint64_t *entry_out);
    
    size_t size;
    void *data = limine_find_module(module_name, &size);
    if (data == NULL) {
        return -1;
    }
    
    struct process *proc;
    uint64_t entry;
    
    /* 从模块名提取程序名 */
    const char *prog_name = module_name;
    const char *slash = module_name;
    while (*slash) {
        if (*slash == '/') prog_name = slash + 1;
        slash++;
    }
    
    if (elf_load_from_memory_ex(data, size, prog_name, &proc, &entry) != 0) {
        return -1;
    }

    /* 如果是 desk 程序，设置 framebuffer 和 logo 映射 */
    {
        int is_desk = 0;
        const char *p = module_name;
        while (*p) {
            if (p[0]=='d' && p[1]=='e' && p[2]=='s' && p[3]=='k') {
                is_desk = 1;
                break;
            }
            p++;
        }

        if (is_desk) {
            extern struct fb_info *fb_get_info(void);
            extern uint64_t vmm_get_physical(struct address_space *space, uint64_t virt);
            extern void *phys_to_virt(uint64_t phys);
            extern int vmm_map_range(struct address_space *space, uint64_t virt, uint64_t phys, size_t pages, uint64_t flags);
            extern void vmm_invalidate_all(void);
            extern uint64_t pmm_alloc_page(void);
            extern void vmm_invalidate_all(void);

            struct fb_info *fb = fb_get_info();
            if (fb && fb->buffer && fb->width && fb->height) {
                /* 映射 framebuffer 到用户空间 0x08000000 */
                uint64_t user_fb_virt = 0x08000000;
                uint64_t fb_phys = fb->physical;
                uint64_t fb_size = fb->size;
                uint64_t fb_pages = (fb_size + 4095) / 4096;
                uint64_t fb_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

                int map_ret = vmm_map_range(proc->space, user_fb_virt, fb_phys, fb_pages, fb_flags);
                if (map_ret == 0) {
                    serial_print("[LIMINE-LOAD] Mapped framebuffer at 0x08000000\n");
                    vmm_invalidate_all();

                    extern uint64_t g_user_fb_virt;
                    g_user_fb_virt = user_fb_virt;
                } else {
                    serial_print("[LIMINE-LOAD] WARNING: Failed to map framebuffer\n");
                }
            }

            /* 映射 back.bmp logo 数据到用户空间 0x0A000000 */
            {
                size_t logo_size;
                void *logo_data = limine_find_module("/system/zrdemage/st/back.bmp", &logo_size);
                if (logo_data && logo_size >= 54) {
                    uint64_t user_logo_virt = 0x0A000000;
                    uint64_t logo_pages = (logo_size + 4095) / 4096;
                    uint64_t logo_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

                    /* 分配新物理页面，复制 logo 数据，映射到用户空间 */
                    for (uint64_t off = 0; off < logo_size; off += 4096) {
                        uint64_t phys = pmm_alloc_page();
                        if (!phys) break;
                        void *page_virt = phys_to_virt(phys);
                        memset(page_virt, 0, 4096);
                        uint64_t chunk = logo_size - off;
                        if (chunk > 4096) chunk = 4096;
                        memcpy(page_virt, (void *)((uint64_t)logo_data + off), chunk);
                        vmm_map_range(proc->space, user_logo_virt + off, phys, 1, logo_flags);
                    }
                    vmm_invalidate_all();
                    serial_print("[LIMINE-LOAD] Mapped back.bmp at 0x0A000000\n");
                }
            }
        }
    }

    struct thread *thread = proc->threads;
    while (thread != NULL) {
        scheduler_add_thread(thread);
        thread = thread->process_next;
    }
    
    return 0;
}

/* 从 VFS 或 Limine 模块加载程序 */
static int load_program(const char *path) {
    serial_print("[LOAD] Loading: ");
    serial_print(path);
    serial_print("\n");
    
    struct process *proc;
    uint64_t entry;
    
    /* 首先尝试从 VFS 加载 */
    serial_print("[LOAD] Trying VFS...\n");
    if (elf_load_file(path, &proc, &entry) == 0) {
        serial_print("[LOAD] VFS OK, proc=0x");
        char buf[20];
        uint64_t addr = (uint64_t)proc;
        int pos = 0;
        for (int i = 60; i >= 0; i -= 4) {
            buf[pos++] = "0123456789ABCDEF"[(addr >> i) & 0xF];
        }
        buf[pos] = '\0';
        serial_print(buf);
        serial_print("\n");

        /* 如果是 desk 程序，设置为普通用户 */
        {
            int is_desk = 0;
            const char *p = path;
            while (*p) {
                if (p[0]=='d' && p[1]=='e' && p[2]=='s' && p[3]=='k') {
                    is_desk = 1;
                    break;
                }
                p++;
            }
            if (is_desk && proc != NULL) {
                extern uint32_t g_boot_cfg_uid;
                extern uint32_t g_boot_cfg_gid;
                extern char g_boot_cfg_username[32];
                proc->uid = g_boot_cfg_uid;
                proc->gid = g_boot_cfg_gid;
                for (int i = 0; i < 32; i++) { proc->username[i] = g_boot_cfg_username[i]; if (!g_boot_cfg_username[i]) break; }
                serial_print("[LOAD] Set desk uid/gid to user account\n");
            }
        }

        /* 如果是 desk 程序，传递 framebuffer 信息 */
        struct fb_info *fb = fb_get_info();
        if (fb && fb->buffer && fb->width && fb->height) {
            /* 检查路径是否包含 "desk" / "zrr" / "windowprogram" (图形程序) */
            const char *p = path;
            int is_desk = 0;
            while (*p) {
                if (p[0]=='d' && p[1]=='e' && p[2]=='s' && p[3]=='k') {
                    is_desk = 1;
                    break;
                }
                if (p[0]=='z' && p[1]=='r' && p[2]=='r') {
                    is_desk = 1;  /* zrrWindowsManager, zrrExfileManager */
                    break;
                }
                if (p[0]=='w' && p[1]=='i' && p[2]=='n' && p[3]=='d' && p[4]=='o' && p[5]=='w' && p[6]=='p') {
                    is_desk = 1;  /* /prgm/windowprogram/* 都是图形程序 */
                    break;
                }
                p++;
            }
            
            if (is_desk) {
                /* 检查是否是 desk 还是 zrr 管理器 */
                const char *pp = path;
                int is_zrr = 0;
                while (*pp) {
                    if (pp[0]=='z' && pp[1]=='r' && pp[2]=='r') {
                        is_zrr = 1;
                        break;
                    }
                    pp++;
                }
                
                if (is_zrr) {
                    serial_print("[LOAD] Setting framebuffer for zrr manager\n");
                } else {
                    serial_print("[LOAD] Setting framebuffer info for desk\n");
                }
                
                /* 获取进程的地址空间 */
                extern uint64_t vmm_get_physical(struct address_space *space, uint64_t virt);
                extern void *phys_to_virt(uint64_t phys);
                extern int vmm_map_range(struct address_space *space, uint64_t virt, uint64_t phys, size_t pages, uint64_t flags);
                extern void vmm_invalidate_all(void);
                
                uint64_t fb_base_addr = elf_lookup_symbol_from_file(path, "fb_base");
                uint64_t fb_width_addr = elf_lookup_symbol_from_file(path, "fb_width");
                uint64_t fb_height_addr = elf_lookup_symbol_from_file(path, "fb_height");
                uint64_t fb_pitch_addr = elf_lookup_symbol_from_file(path, "fb_pitch");
                
                if (!fb_base_addr || !fb_width_addr || !fb_height_addr || !fb_pitch_addr) {
                    serial_print("[LOAD] WARNING: Failed to lookup fb symbols in desk ELF\n");
                }
                
                /* 将 framebuffer 物理地址映射到用户进程的虚拟地址空间 */
                uint64_t user_fb_virt;
                if (is_zrr) {
                    /* zrr 管理器期望 fb 在 0xFD000000 */
                    user_fb_virt = 0xFD000000;
                } else {
                    /* desk 使用 0x08000000 */
                    user_fb_virt = 0x08000000;
                }
                uint64_t fb_phys = fb->physical;
                uint64_t fb_size = fb->size;
                uint64_t fb_pages = (fb_size + 4095) / 4096;
                
                /* 确保至少映射 1024 页（4MB），足够覆盖 1280x800x4 */
                if (fb_pages < 1024) fb_pages = 1024;
                
                serial_print("[LOAD] FB: phys=0x");
                char fbphys[20]; int fbph = 0;
                for (int i = 60; i >= 0; i -= 4) fbphys[fbph++] = "0123456789ABCDEF"[(fb_phys >> i) & 0xF];
                fbphys[fbph] = '\n'; fbphys[fbph+1] = '\0';
                serial_print(fbphys);
                
                /* 打印内核 fb.buffer 的 HHDM 虚拟地址 */
                serial_print("[LOAD] FB: kernel virt (HHDM)=0x");
                char hhdm[20]; int hh = 0;
                uint64_t hhdm_val = (uint64_t)fb->buffer;
                for (int i = 60; i >= 0; i -= 4) hhdm[hh++] = "0123456789ABCDEF"[(hhdm_val >> i) & 0xF];
                hhdm[hh] = '\n'; hhdm[hh+1] = '\0';
                serial_print(hhdm);
                
                serial_print("[LOAD] FB: size=");
                char fbsize[16]; int fbsp = 0;
                uint64_t fbsv = fb_size;
                if (fbsv == 0) fbsize[fbsp++] = '0';
                else { char fst[16]; int fstp = 0; while (fbsv > 0) { fst[fstp++] = '0' + (fbsv % 10); fbsv /= 10; } while (fstp > 0) fbsize[fbsp++] = fst[--fstp]; }
                fbsize[fbsp] = '\n'; fbsize[fbsp+1] = '\0';
                serial_print(fbsize);
                
                serial_print("[LOAD] FB: pages=");
                char fbpg[16]; int fbpgp = 0;
                uint64_t fbpgv = fb_pages;
                if (fbpgv == 0) fbpg[fbpgp++] = '0';
                else { char fbpgt[16]; int fbpgtp = 0; while (fbpgv > 0) { fbpgt[fbpgtp++] = '0' + (fbpgv % 10); fbpgv /= 10; } while (fbpgtp > 0) fbpg[fbpgp++] = fbpgt[--fbpgtp]; }
                fbpg[fbpgp] = '\n'; fbpg[fbpgp+1] = '\0';
                serial_print(fbpg);
                
                /* 映射 framebuffer 到用户空间 */
                /* 使用标准用户空间标志 */
                uint64_t fb_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
                int map_ret = vmm_map_range(proc->space, user_fb_virt, fb_phys, fb_pages, fb_flags);
                if (map_ret == 0) {
                    serial_print("[LOAD] Mapped framebuffer at user virt 0x");
                    char fbhex[20]; int fbh = 0;
                    for (int i = 60; i >= 0; i -= 4) fbhex[fbh++] = "0123456789ABCDEF"[(user_fb_virt >> i) & 0xF];
                    fbhex[fbh] = '\n'; fbhex[fbh+1] = '\0';
                    serial_print(fbhex);
                    vmm_invalidate_all();
                    
                    /* 保存用户空间 framebuffer 地址供内核刷新使用
                     * 注意：只在加载 desk 时更新，避免 zrr 覆盖 desk 的地址
                     */
                    if (!is_zrr) {
                        extern uint64_t g_user_fb_virt;
                        g_user_fb_virt = user_fb_virt;
                        serial_print("[LOAD] Set g_user_fb_virt=0x");
                        char gv[20]; int gvp = 0;
                        for (int i = 60; i >= 0; i -= 4) gv[gvp++] = "0123456789ABCDEF"[(user_fb_virt >> i) & 0xF];
                        gv[gvp] = '\n'; serial_print(gv);
                    }
                    
                    /* 验证映射：从用户页表读取物理地址 */
                    extern uint64_t vmm_get_physical(struct address_space *space, uint64_t virt);
                    uint64_t verify_phys = vmm_get_physical(proc->space, user_fb_virt);
                    serial_print("[LOAD] Verify: user_virt 0x");
                    char vh[20]; int vhp = 0;
                    for (int i = 60; i >= 0; i -= 4) vh[vhp++] = "0123456789ABCDEF"[(user_fb_virt >> i) & 0xF];
                    vh[vhp] = '\0'; serial_print(vh);
                    serial_print(" -> phys 0x");
                    vhp = 0;
                    for (int i = 60; i >= 0; i -= 4) vh[vhp++] = "0123456789ABCDEF"[(verify_phys >> i) & 0xF];
                    vh[vhp] = '\n'; serial_print(vh);
                } else {
                    serial_print("[LOAD] WARNING: Failed to map framebuffer (ret=");
                    char rbuf[16]; rbuf[0] = '0' + map_ret; rbuf[1] = '\n'; rbuf[2] = '\0';
                    serial_print(rbuf);
                }
                
                /* 写入 fb 变量 (仅 desk 程序，zrr 管理器不需要) */
                if (!is_zrr) {
                /* 写入 fb_base (uint32_t*) - 使用用户空间虚拟地址 */
                uint64_t fb_base_phys = vmm_get_physical(proc->space, fb_base_addr);
                serial_print("[LOAD] fb_base_phys=0x");
                { char bp[20]; int bpi = 0; for (int i = 60; i >= 0; i -= 4) bp[bpi++] = "0123456789ABCDEF"[(fb_base_phys >> i) & 0xF]; bp[bpi] = '\n'; serial_print(bp); }
                if (fb_base_phys) {
                    *(uint64_t *)phys_to_virt(fb_base_phys) = user_fb_virt;
                    /* 刷新 TLB 确保用户进程看到新值 */
                    extern void vmm_invalidate_all(void);
                    vmm_invalidate_all();
                    serial_print("[LOAD] Set fb_base=0x");
                    char hexbuf[20]; int hp = 0;
                    uint64_t hv = user_fb_virt;
                    for (int i = 60; i >= 0; i -= 4) hexbuf[hp++] = "0123456789ABCDEF"[(hv >> i) & 0xF];
                    hexbuf[hp] = '\n'; hexbuf[hp+1] = '\0';
                    serial_print(hexbuf);
                }
                
                /* 写入 fb_width (uint32_t) */
                uint64_t fb_width_phys = vmm_get_physical(proc->space, fb_width_addr);
                if (fb_width_phys) {
                    *(uint32_t *)phys_to_virt(fb_width_phys) = fb->width;
                    serial_print("[LOAD] Set fb_width=");
                    char wbuf[16]; int wp = 0;
                    uint32_t wv = fb->width;
                    if (wv == 0) wbuf[wp++] = '0';
                    else { char wt[16]; int wtp = 0; while (wv > 0) { wt[wtp++] = '0' + (wv % 10); wv /= 10; } while (wtp > 0) wbuf[wp++] = wt[--wtp]; }
                    wbuf[wp] = '\n'; wbuf[wp+1] = '\0';
                    serial_print(wbuf);
                }
                
                /* 写入 fb_height (uint32_t) */
                uint64_t fb_height_phys = vmm_get_physical(proc->space, fb_height_addr);
                if (fb_height_phys) {
                    *(uint32_t *)phys_to_virt(fb_height_phys) = fb->height;
                    serial_print("[LOAD] Set fb_height=");
                    char hbuf[16]; int hhp = 0;
                    uint32_t hv2 = fb->height;
                    if (hv2 == 0) hbuf[hhp++] = '0';
                    else { char ht[16]; int htp = 0; while (hv2 > 0) { ht[htp++] = '0' + (hv2 % 10); hv2 /= 10; } while (htp > 0) hbuf[hhp++] = ht[--htp]; }
                    hbuf[hhp] = '\n'; hbuf[hhp+1] = '\0';
                    serial_print(hbuf);
                }
                
                /* 写入 fb_pitch (uint32_t) */
                uint64_t fb_pitch_phys = vmm_get_physical(proc->space, fb_pitch_addr);
                if (fb_pitch_phys) {
                    *(uint32_t *)phys_to_virt(fb_pitch_phys) = fb->pitch;
                    serial_print("[LOAD] Set fb_pitch=");
                    char pbuf[16]; int pp = 0;
                    uint32_t pv = fb->pitch;
                    if (pv == 0) pbuf[pp++] = '0';
                    else { char pt[16]; int ptp = 0; while (pv > 0) { pt[ptp++] = '0' + (pv % 10); pv /= 10; } while (ptp > 0) pbuf[pp++] = pt[--ptp]; }
                    pbuf[pp] = '\n'; pbuf[pp+1] = '\0';
                    serial_print(pbuf);
                }
                } /* end !is_zrr */
                
                /* 加载 back.bmp 并传递给 desk 作为背景 */
                extern void *limine_find_module(const char *path, size_t *size_out);
                size_t logo_size;
                void *logo_data = limine_find_module("/system/zrdemage/st/back.bmp", &logo_size);
                if (logo_data) {
                    serial_print("[LOAD] Found back.bmp, size=");
                    char lbuf[16]; int lp = 0;
                    if (logo_size == 0) lbuf[lp++] = '0';
                    else {
                        uint64_t tmp = logo_size;
                        char tbuf[16]; int t = 0;
                        while (tmp > 0) { tbuf[t++] = '0' + (tmp % 10); tmp /= 10; }
                        while (t > 0) lbuf[lp++] = tbuf[--t];
                    }
                    lbuf[lp] = '\n'; lbuf[lp+1] = '\0';
                    serial_print(lbuf);
                    
                    /* 解析 BMP 头部获取真实尺寸 */
                    if (logo_size >= 54) {
                        unsigned char *bmp = (unsigned char *)logo_data;
                        uint32_t bmp_width = 0;
                        uint32_t bmp_height = 0;
                        bmp_width = (uint32_t)bmp[18] | ((uint32_t)bmp[19] << 8) | ((uint32_t)bmp[20] << 16) | ((uint32_t)bmp[21] << 24);
                        bmp_height = (uint32_t)bmp[22] | ((uint32_t)bmp[23] << 8) | ((uint32_t)bmp[24] << 16) | ((uint32_t)bmp[25] << 24);
                        
                        /* 高度可能是负数（绝对值） */
                        if (bmp_height & 0x80000000) bmp_height = bmp_height & 0x7FFFFFFF;
                        
                        serial_print("[LOAD] BMP dimensions: ");
                        char dimbuf[32]; int dip = 0;
                        uint32_t dw = bmp_width;
                        if (dw == 0) dimbuf[dip++] = '0';
                        else { char dt[16]; int tp = 0; while (dw > 0) { dt[tp++] = '0' + (dw % 10); dw /= 10; } while (tp > 0) dimbuf[dip++] = dt[--tp]; }
                        dimbuf[dip++] = 'x';
                        dw = bmp_height;
                        if (dw == 0) dimbuf[dip++] = '0';
                        else { char dt[16]; int tp = 0; while (dw > 0) { dt[tp++] = '0' + (dw % 10); dw /= 10; } while (tp > 0) dimbuf[dip++] = dt[--tp]; }
                        dimbuf[dip] = '\n'; dimbuf[dip+1] = '\0';
                        serial_print(dimbuf);
                        
                        uint64_t logo_data_addr = elf_lookup_symbol_from_file(path, "logo_data");
                        uint64_t logo_width_addr = elf_lookup_symbol_from_file(path, "logo_width");
                        uint64_t logo_height_addr = elf_lookup_symbol_from_file(path, "logo_height");
                        uint64_t logo_data_offset_addr = elf_lookup_symbol_from_file(path, "logo_data_offset");
                        
                        if (!logo_data_addr || !logo_width_addr || !logo_height_addr || !logo_data_offset_addr) {
                            serial_print("[LOAD] WARNING: Failed to lookup logo symbols in desk ELF\n");
                        }
                        
                        /* 检查 logo 文件尺寸是否符合预期 */
                        if (logo_size >= 1024 && bmp_width > 0 && bmp_height > 0) {
                            serial_print("[LOAD] Logo size OK, passing to desk\n");
                            
                            /* 分配用户空间页面存储 logo 数据 */
                            uint64_t user_logo_virt = 0x0A000000; /* 160MB - 用户空间 */
                            uint64_t logo_pages = (logo_size + 4095) / 4096;
                            
                            /* 分配物理页面 */
                            extern uint64_t pmm_alloc_pages(size_t count);
                            uint64_t logo_phys_addr = pmm_alloc_pages(logo_pages);
                            
                            if (logo_phys_addr) {
                                /* 映射到用户空间 */
                                uint64_t logo_map_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
                                int logo_map_ret = vmm_map_range(proc->space, user_logo_virt, logo_phys_addr, logo_pages, logo_map_flags);
                                
                                if (logo_map_ret == 0) {
                                    serial_print("[LOAD] Mapped logo at user virt 0x");
                                    char luhex[20]; int luhp = 0;
                                    for (int i = 60; i >= 0; i -= 4) luhex[luhp++] = "0123456789ABCDEF"[(user_logo_virt >> i) & 0xF];
                                    luhex[luhp] = '\n'; luhex[luhp+1] = '\0';
                                    serial_print(luhex);
                                    
                                    /* 复制 logo 数据到用户空间 */
                                    void *logo_kern_dest = phys_to_virt(logo_phys_addr);
                                    memcpy(logo_kern_dest, logo_data, logo_size);
                                    
                                    /* 设置 logo 数据指针为用户空间地址 */
                                    uint64_t logo_data_phys = vmm_get_physical(proc->space, logo_data_addr);
                                    serial_print("[LOAD] logo_data_phys=");
                                    { char lp[20]; int lpi = 0; for (int i = 60; i >= 0; i -= 4) lp[lpi++] = "0123456789ABCDEF"[(logo_data_phys >> i) & 0xF]; lp[lpi] = '\n'; serial_print(lp); }
                                    if (logo_data_phys) {
                                        *(uint64_t *)phys_to_virt(logo_data_phys) = user_logo_virt;
                                        serial_print("[LOAD] Set logo_data=0x");
                                        char lhex[20]; int lhp = 0;
                                        uint64_t logo_ptr = user_logo_virt;
                                        for (int i = 60; i >= 0; i -= 4) lhex[lhp++] = "0123456789ABCDEF"[(logo_ptr >> i) & 0xF];
                                        lhex[lhp] = '\n'; lhex[lhp+1] = '\0';
                                        serial_print(lhex);
                                        
                                        /* Verify write */
                                        uint64_t verify_logo = *(uint64_t *)phys_to_virt(logo_data_phys);
                                        serial_print("[LOAD] Verify logo_data=");
                                        { char vh[20]; int vi = 0; for (int i = 60; i >= 0; i -= 4) vh[vi++] = "0123456789ABCDEF"[(verify_logo >> i) & 0xF]; vh[vi] = '\n'; serial_print(vh); }
                                    }
                                } else {
                                    serial_print("[LOAD] WARNING: Failed to map logo data\n");
                                }
                            } else {
                                serial_print("[LOAD] WARNING: Failed to allocate logo pages\n");
                            }
                            
                            /* 设置 logo 尺寸（从 BMP 头部解析） */
                            uint64_t logo_w_phys = vmm_get_physical(proc->space, logo_width_addr);
                            if (logo_w_phys) {
                                *(uint32_t *)phys_to_virt(logo_w_phys) = bmp_width;
                                serial_print("[LOAD] Set logo_width=");
                                char lwbuf[16]; int lwp = 0;
                                uint32_t lw = bmp_width;
                                if (lw == 0) lwbuf[lwp++] = '0';
                                else { char lwt[16]; int ltp = 0; while (lw > 0) { lwt[ltp++] = '0' + (lw % 10); lw /= 10; } while (ltp > 0) lwbuf[lwp++] = lwt[--ltp]; }
                                lwbuf[lwp] = '\n'; lwbuf[lwp+1] = '\0';
                                serial_print(lwbuf);
                            }
                            
                            uint64_t logo_h_phys = vmm_get_physical(proc->space, logo_height_addr);
                            if (logo_h_phys) {
                                *(uint32_t *)phys_to_virt(logo_h_phys) = bmp_height;
                                serial_print("[LOAD] Set logo_height=");
                                char lhbuf[16]; int lhp2 = 0;
                                uint32_t lh = bmp_height;
                                if (lh == 0) lhbuf[lhp2++] = '0';
                                else { char lht[16]; int ltp = 0; while (lh > 0) { lht[ltp++] = '0' + (lh % 10); lh /= 10; } while (ltp > 0) lhbuf[lhp2++] = lht[--ltp]; }
                                lhbuf[lhp2] = '\n'; lhbuf[lhp2+1] = '\0';
                                serial_print(lhbuf);
                            }
                            
                            /* 设置 logo_data_offset（从 BMP 头部字节 10-13 读取） */
                            uint32_t bmp_data_offset = (uint32_t)bmp[10] | ((uint32_t)bmp[11] << 8) | ((uint32_t)bmp[12] << 16) | ((uint32_t)bmp[13] << 24);
                            uint64_t logo_off_phys = vmm_get_physical(proc->space, logo_data_offset_addr);
                            if (logo_off_phys) {
                                *(uint32_t *)phys_to_virt(logo_off_phys) = bmp_data_offset;
                                serial_print("[LOAD] Set logo_data_offset=");
                                char lobuf[16]; int lop = 0;
                                uint32_t lo = bmp_data_offset;
                                if (lo == 0) lobuf[lop++] = '0';
                                else { char lot[16]; int ltp = 0; while (lo > 0) { lot[ltp++] = '0' + (lo % 10); lo /= 10; } while (ltp > 0) lobuf[lop++] = lot[--ltp]; }
                                lobuf[lop] = '\n'; lobuf[lop+1] = '\0';
                                serial_print(lobuf);
                            }
                        } else {
                            serial_print("[LOAD] WARNING: Logo file too small or invalid dimensions\n");
                        }
                    } else {
                        serial_print("[LOAD] WARNING: Logo file too small for BMP\n");
                    }
                } else {
                    serial_print("[LOAD] logo.bmp not found in Limine modules\n");
                }
                
            }
        }
        
        int count = 0;
        struct thread *thread = proc->threads;
        while (thread != NULL) {
            serial_print("[LOAD] Added thread tid=");
            int n = thread->tid;
            char nbuf[12];
            int pos = 0;
            if (n == 0) nbuf[pos++] = '0';
            else {
                char tmp[12];
                int t = 0;
                while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
                while (t > 0) nbuf[pos++] = tmp[--t];
            }
            nbuf[pos] = '\0';
            serial_print(nbuf);
            serial_print("\n");
            
            scheduler_add_thread(thread);
            count++;
            thread = thread->process_next;
        }
        serial_print("[LOAD] Total threads added: ");
        int n = count;
        pos = 0;
        if (n == 0) buf[pos++] = '0';
        else {
            char tmp[12];
            int t = 0;
            while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            while (t > 0) buf[pos++] = tmp[--t];
        }
        buf[pos] = '\0';
        serial_print(buf);
        serial_print("\n");
        return 0;
    }
    
    /* VFS 加载失败，尝试从 Limine 模块加载 */
    serial_print("[LOAD] VFS failed, trying Limine modules...\n");
    
    if (load_elf_from_limine(path) == 0) {
        serial_print("[LOAD] Limine module loaded successfully\n");
        return 0;
    }
    
    /* 两种方式都失败，列出可用模块 */
    serial_print("[LOAD] ERROR: Failed to load from both VFS and Limine\n");
    serial_print("[LOAD] Available Limine modules:\n");
    int mod_count = limine_get_module_count();
    serial_print("[LOAD] Module count: ");
    char mbuf[8];
    int p = 0;
    if (mod_count == 0) mbuf[p++] = '0';
    else {
        int tmp = mod_count;
        char tbuf[8];
        int t = 0;
        while (tmp > 0) { tbuf[t++] = '0' + (tmp % 10); tmp /= 10; }
        while (t > 0) mbuf[p++] = tbuf[--t];
    }
    mbuf[p] = '\0';
    serial_print(mbuf);
    serial_print("\n");
    
    for (int i = 0; i < mod_count; i++) {
        size_t mod_size;
        const char *mod_path;
        limine_get_module(i, &mod_size, &mod_path);
        if (mod_path) {
            serial_print("  [");
            char ibuf[8];
            int ip = 0;
            if (i == 0) ibuf[ip++] = '0';
            else {
                int tmp = i;
                char tbuf[8];
                int t = 0;
                while (tmp > 0) { tbuf[t++] = '0' + (tmp % 10); tmp /= 10; }
                while (t > 0) ibuf[ip++] = tbuf[--t];
            }
            ibuf[ip] = '\0';
            serial_print(ibuf);
            serial_print("] ");
            serial_print(mod_path);
            serial_print("\n");
        }
    }
    
    return -1;
}

/* ========== 自主文件系统：RAMFS 默认文件生成器 ==========
 * 即使没有外部磁盘或 Limine 模块，也能提供完整的文件系统
 * 策略：先检查文件是否存在，不存在才创建默认版本
 * 这样 Limine 传递的文件会优先使用
 */
typedef struct {
    const char *path;
    const char *content;
    uint32_t uid, gid;
    uint32_t mode;
} default_file_t;

static void ramfs_ensure_file(const char *path, const char *content,
                               uint32_t uid, uint32_t gid) {
    struct vfs_stat st;
    if (vfs_stat(path, &st) == 0 && st.size > 0) {
        serial_print("[AUTOFS] Exists: ");
        serial_print(path);
        serial_print("\n");
        return;
    }
    struct vfs_file *f = vfs_open(path, VFS_MODE_WRITE | VFS_MODE_CREATE);
    if (!f) {
        serial_print("[AUTOFS] FAIL create: ");
        serial_print(path);
        serial_print("\n");
        return;
    }
    size_t clen = 0;
    while (content[clen]) clen++;
    if (clen > 0) vfs_write(f, (void*)content, clen);
    vfs_close(f);
    vfs_chown(path, uid, gid);
    serial_print("[AUTOFS] Created: ");
    serial_print(path);
    serial_print(" (");
    { char nb[16]; int np=0; uint64_t t=clen; if(t==0){nb[np++]='0';}else{char tb[16];int tp=0;while(t>0){tb[tp++]='0'+(t%10);t/=10;}while(tp>0)nb[np++]=tb[--tp];}nb[np]=0;serial_print(nb);}
    serial_print(" bytes)\n");
}

static void ramfs_populate_defaults(uint32_t uid, uint32_t gid, const char *username) {
    serial_print("[AUTOFS] Populating default filesystem...\n");

    /* 1. 桌面配置 */
    ramfs_ensure_file("/desktop/desktop.conf",
        "back=\\usr\\user\\video\\back.mp4\n", 0, 0);

    /* 2. Root 用户桌面图标 */
    ramfs_ensure_file("/desktop/root/Files.prd",
        "/prgm/windowprogram/fm\n", 0, 0);
    ramfs_ensure_file("/desktop/root/Terminal.prd",
        "/prgm/shell\n", 0, 0);
    ramfs_ensure_file("/desktop/root/Software.prd",
        "/prgm/windowprogram/zinstall\n", 0, 0);

    /* 3. 普通用户桌面图标 */
    ramfs_ensure_file("/desktop/user/Files.prd",
        "/prgm/windowprogram/fm\n", uid, gid);
    ramfs_ensure_file("/desktop/user/Terminal.prd",
        "/prgm/shell\n", uid, gid);
    ramfs_ensure_file("/desktop/user/Software.prd",
        "/prgm/windowprogram/zinstall\n", uid, gid);

    /* 4. 用户配置（如果不存在或缺少 passwd 键）*/
    {
        struct vfs_stat st;
        int need_create = 0;
        int need_passwd = 0;

        if (vfs_stat("/ini/usrroot/user.cfg", &st) != 0 || st.size == 0) {
            need_create = 1;
        } else {
            /* 检查现有文件是否包含 passwd 键 */
            struct vfs_file *f = vfs_open("/ini/usrroot/user.cfg", VFS_MODE_READ);
            if (f != NULL) {
                char buf[512];
                ssize_t n = vfs_read(f, buf, sizeof(buf) - 1);
                vfs_close(f);
                if (n > 0) {
                    buf[n] = 0;
                    /* 搜索 "passwd=" 子串 */
                    int found = 0;
                    for (int i = 0; i <= n - 7; i++) {
                        if (buf[i] == 'p' && buf[i+1] == 'a' && buf[i+2] == 's' &&
                            buf[i+3] == 's' && buf[i+4] == 'w' && buf[i+5] == 'd' &&
                            buf[i+6] == '=') {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        need_passwd = 1;
                    }
                }
            }
        }

        if (need_passwd) {
            /* 追加 passwd 键到现有文件 */
            struct vfs_file *f = vfs_open("/ini/usrroot/user.cfg", VFS_MODE_WRITE | VFS_MODE_READ);
            if (f != NULL) {
                vfs_seek_ext(f, 0, 2);  /* SEEK_END */
                const char *passwd_line = "passwd=e10adc3949ba59abbe56e057f20f883e\n";
                vfs_write(f, passwd_line, 40);
                vfs_close(f);
            }
        }

        if (need_create) {
            char ucfg[256];
            int up = 0;
            const char *fmt_uid = "uid=0\ngid=0\nname=root\nhome=/root\npasswd=e10adc3949ba59abbe56e057f20f883e\n\nuid=";
            while (*fmt_uid) ucfg[up++] = *fmt_uid++;
            { char nb[16]; int np=0; uint64_t t=uid; if(t==0){nb[np++]='0';}else{char tb[16];int tp=0;while(t>0){tb[tp++]='0'+(t%10);t/=10;}while(tp>0)nb[np++]=tb[--tp];}nb[np]=0;const char*p=nb;while(*p)ucfg[up++]=*p++;}
            ucfg[up++] = '\n';
            const char *fmt_gid = "\ngid=";
            while (*fmt_gid) ucfg[up++] = *fmt_gid++;
            { char nb[16]; int np=0; uint64_t t=gid; if(t==0){nb[np++]='0';}else{char tb[16];int tp=0;while(t>0){tb[tp++]='0'+(t%10);t/=10;}while(tp>0)nb[np++]=tb[--tp];}nb[np]=0;const char*p=nb;while(*p)ucfg[up++]=*p++;}
            ucfg[up++] = '\n';
            const char *fmt_name = "\nname=";
            while (*fmt_name) ucfg[up++] = *fmt_name++;
            const char *un = username ? username : "user";
            while (*un && up < 250) ucfg[up++] = *un++;
            ucfg[up++] = '\n';
            const char *fmt_home = "home=/desktop/user\npasswd=e10adc3949ba59abbe56e057f20f883e\n";
            while (*fmt_home && up < 255) ucfg[up++] = *fmt_home++;
            ucfg[up] = 0;
            ramfs_ensure_file("/ini/usrroot/user.cfg", ucfg, 0, 0);
        }
    }

    /* 5. 网络配置 */
    {
        struct vfs_stat st;
        if (vfs_stat("/system/internet/e1w.cfg", &st) != 0 || st.size == 0) {
            ramfs_ensure_file("/system/internet/e1w.cfg",
                "name = \"\"\npassword = \"\"\n", 0, 0);
        }
    }

    /* 6. AI 配置占位 */
    ramfs_ensure_file("/zrr64/zdonk/AI/APIKey.confai",
        "# ZratroOS AI Configuration\nservice=\napi_key=\nendpoint=\nmodel=\n",
        uid, gid);

    /* 7. ZTS 构建脚本占位 */
    ramfs_ensure_file("/zrr64/zdonk/code/build.bs",
        "# ZratroOS Build Script\n", uid, gid);

    serial_print("[AUTOFS] Default filesystem populated.\n");
}

static void launch_programs(void) {
    /* Disable console output to screen - only serial/klog from now on
     * to avoid debug text appearing over the desktop GUI */
    console_disable_output();

    serial_print("[BOOT] launch_programs() called\n");

    /* Debug: print config file path */
    serial_print("[BOOT] Config file path: ");
    serial_print(CONFIG_FILE_PATH);
    serial_print("\n");

    struct config_file *config = config_load(CONFIG_FILE_PATH);
    {
        char abuf[20];
        uint64_t addr = (uint64_t)config;
        int p = 0;
        for (int i = 60; i >= 0; i -= 4)
            abuf[p++] = "0123456789ABCDEF"[(addr >> i) & 0xF];
        abuf[p] = '\0';
        serial_print("[BOOT] config = 0x");
        serial_print(abuf);
        serial_print("\n");
    }
    
    if (config == NULL) {
        serial_print("[BOOT] Config load failed, falling back to direct shell launch\n");
    } else {
        char programs[16][256];
        int num_programs = config_get_programs(config, programs, 16);
        
        {
            char nbuf[12];
            int n = num_programs, p = 0;
            if (n == 0) nbuf[p++] = '0';
            else { char tmp[12]; int t = 0; while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; } while (t > 0) nbuf[p++] = tmp[--t]; }
            nbuf[p] = '\0';
            serial_print("[BOOT] num_programs = ");
            serial_print(nbuf);
            serial_print("\n");
        }

        if (num_programs == 0) {
            serial_print("[BOOT] num_programs is 0, trying shell\n");
            config_free(config);
        } else {
            serial_print("[BOOT] Loading programs from config\n");
            for (int i = 0; i < num_programs; i++) {
                serial_print("[BOOT] Loading: ");
                serial_print(programs[i]);
                serial_print("\n");
                if (load_program(programs[i]) != 0) {
                    serial_print("[BOOT] ERROR: Failed to load: ");
                    serial_print(programs[i]);
                    serial_print("\n");
                }
            }
            config_free(config);
            return;
        }
    }

    /* Launch desktop, fallback to shell */
    serial_print("[BOOT] Launching desktop...\n");
    if (load_program("/prgm/sys/desk") != 0) {
        serial_print("[BOOT] ERROR: Failed to load desk!\n");
        if (load_program("/prgm/sys/shell") != 0) {
            serial_print("[BOOT] ERROR: Also failed to load shell!\n");
        }
    }
}

/* ========== 内核主函数 ========== */



/* kernel_main 函数禁用栈保护，因为在 GDT 初始化之前 gs 段基址未设置
 * 编译器 -fstack-protector-strong 会在函数开头插入读取 gs:0x28 的代码
 * 如果 gs 未设置，会导致段错误和 triple fault 重启
 */
__attribute__((no_stack_protector))
void kernel_main(void) {
    /* ========== 关键：立即设置 GS 基址 ==========
     * 必须在任何函数调用之前完成！
     * GCC -fstack-protector-strong 会访问 gs:0x28 读取栈 canary
     * 如果 gs 基址未设置，会导致访问线性地址 0x28 并触发页面错误
     */
    extern struct cpu_local cpu_data[];
    
    /* 使用 TSC 初始化栈 canary */
    uint64_t tsc_lo, tsc_hi;
    asm volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
    uint64_t tsc = (tsc_hi << 32) | tsc_lo;
    uint64_t canary = tsc ^ (tsc >> 32) ^ 0xDEADBEEFCAFEBABEULL;
    
    /* 初始化 cpu_data[0] 的基本字段 */
    cpu_data[0].cpu_id = 0;
    cpu_data[0].apic_id = 0;  /* 稍后由 smp_init 更新 */
    cpu_data[0].state = CPU_STATE_RUNNING;
    cpu_data[0].stack_canary = canary;
    
    /* 设置 GS 基址指向 cpu_data[0] 
     * 在 Higher Half 内核中，确保使用正确的虚拟地址 */
    uint64_t gs_base = (uint64_t)&cpu_data[0];
    
    /* 安全检查 */
    if (gs_base == 0) {
        kernel_panic("cpu_data address is NULL");
    }
    
    if (gs_base < 0xFFFFFFFF80000000ULL) {
        kernel_panic("cpu_data address not in Higher Half");
    }
    
    if (gs_base & 0xFFF) {
        kernel_panic("cpu_data address not page-aligned");
    }
    
    volatile uint64_t test_read = cpu_data[0].cpu_id;
    (void)test_read;
    
    asm volatile("wrmsr" : :
        "c"((uint32_t)0xC0000101),  /* IA32_GS_BASE */
        "a"((uint32_t)gs_base),
        "d"((uint32_t)(gs_base >> 32)));
    
    /* 验证 GS 设置 */
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"((uint32_t)0xC0000101));
    uint64_t gs_verify = ((uint64_t)high << 32) | low;
    
    if (gs_verify != gs_base) {
        kernel_panic("GS register verification failed");
    }

    /* ========== 阶段 1: Limine 引导验证 ========== */
    if (fb_request.response == NULL) {
        kernel_panic("Framebuffer request failed");
    }
    
    if (fb_request.response->framebuffer_count == 0) {
        kernel_panic("No framebuffers available");
    }
    
    if (fb_request.response->framebuffers[0] == NULL) {
        kernel_panic("First framebuffer is NULL");
    }
    
    int w = fb_request.response->framebuffers[0]->width;
    int h = fb_request.response->framebuffers[0]->height;
    (void)w; (void)h;  /* 抑制未使用警告 */
    
    if (w == 0 || h == 0) {
        kernel_panic("Invalid framebuffer dimensions");
    }

    if (memmap_request.response == NULL) {
        kernel_panic("Memory map request failed");
    }
    
    if (memmap_request.response->entry_count == 0) {
        kernel_panic("No memory map entries available");
    }

    if (hhdm_request.response == NULL) {
        kernel_panic("HHDM request failed");
    }
    
    if (hhdm_request.response->offset == 0) {
        kernel_panic("HHDM offset is 0");
    }

    serial_print("[KM] 1:cpu_init\n");
    cpu_init();
    serial_print("[KM] 2:cpu_enable_security_features\n");
    cpu_enable_security_features();
    serial_print("[KM] 3:stack_protection_init\n");
    stack_protection_init();

    /* ========== 阶段 2: 核心初始化 ========== */
    serial_print("[KM] Phase2 start\n");
    screen_info("Phase 2: Core init (GDT/IDT/PMM/VMM/Heap)");
    serial_print("[KM] 4:gdt_init\n");
    screen_info("  4: gdt_init...");
    gdt_init();
    serial_print("[KM] 5:idt_init\n");
    screen_info("  5: idt_init...");
    idt_init();
    serial_print("[KM] 6:pmm_init\n");
    screen_info("  6: pmm_init...");
    pmm_init(memmap_request.response, hhdm_request.response);
    serial_print("[KM] 7:vmm_init\n");
    screen_info("  7: vmm_init...");
    vmm_init();
    serial_print("[KM] 8:heap_init\n");
    screen_info("  8: heap_init...");
    heap_init();
    serial_print("[KM] 9:klog_init\n");
    screen_info("  9: klog_init...");
    klog_init();  /* 尽早初始化日志，捕获后续错误 */
    serial_print("[KM] 10:apic_init\n");
    screen_info(" 10: apic_init...");
    apic_init();
    serial_print("[KM] 11:smp_init\n");
    screen_info(" 11: smp_init...");
    smp_init();
    serial_print("[KM] 12:parse_cmdline\n");
    screen_info(" 12: parse_cmdline...");
    parse_cmdline();
    screen_ok("Phase 2 complete");

    /* 注册中断处理程序 */
    isr_register_handler(APIC_TIMER_VECTOR, apic_timer_handler);
    isr_register_handler(APIC_SPURIOUS_VECTOR, apic_spurious_handler);
    isr_register_handler(APIC_ERROR_VECTOR, apic_error_handler);
    isr_register_handler(IPI_RESCHEDULE, smp_reschedule_handler);
    isr_register_handler(IPI_CALL_FUNCTION, smp_call_function_interrupt);
    isr_register_handler(IPI_CPU_STOP, smp_cpu_stop_handler);
    isr_register_handler(IPI_TLB_SHOOTDOWN, smp_tlb_shootdown_handler);
    isr_register_handler(IPI_MEM_BARRIER, smp_mem_barrier_handler);

    /* ========== 阶段 3: 设备与文件系统 ========== */
    serial_print("[KM] Phase3 start\n");
    screen_info("Phase 3: Devices & Filesystem");
    /* 输入子系统 - 必须在 PS/2 之前初始化 */
    serial_print("[KM] 13:input_init\n");
    input_init();
    /* IOAPIC - 必须在 PS/2 之前初始化（PS/2 需要 IOAPIC 路由 IRQ） */
    serial_print("[KM] 14:ioapic_init\n");
    ioapic_init();
    /* PS/2 输入设备 (键盘/鼠标) - 在 USB 之前初始化，避免 USB 崩溃导致无输入 */
    serial_print("[KM] 15:ps2_init\n");
    ps2_init();

    serial_print("[KM] 16:limine_file_init\n");
    limine_file_init();
    serial_print("[KM] 17:nvme_init\n");
    nvme_init();  /* 初始化 NVMe 存储 */
    serial_print("[KM] 18:disk_init\n");
    disk_init();
    serial_print("[KM] 19:vfs_init\n");
    vfs_init();

    /* 初始化 TTY 子系统 */
    tty_init();

    int fat32_ret = fat32_init();
    if (fat32_ret == 0) screen_ok("FAT32 mounted");
    else screen_warn("FAT32 not found, using RAMFS fallback");
    serial_print("[BOOT] fat32_init returned: ");
    {
        char buf[16]; int p = 0;
        uint64_t tmp = fat32_ret;
        if (tmp == 0) buf[p++] = '0';
        else {
            char t[16]; int tp = 0;
            while (tmp > 0) { t[tp++] = '0' + (tmp % 10); tmp /= 10; }
            while (tp > 0) buf[p++] = t[--tp];
        }
        buf[p] = '\n'; buf[p+1] = '\0';
        serial_print(buf);
    }
    if (fat32_ret != 0) {
        /* 没有检测到可用的 FAT32 文件系统，使用 ramfs 作为回退存储 */
        serial_print("[BOOT] No FAT32 filesystem found, using RAMFS fallback\n");
        extern int ramfs_init(void);
        extern int ramfs_mount(const char *path);
        
        serial_print("[BOOT] Calling ramfs_init...\n");
        ramfs_init();
        serial_print("[BOOT] ramfs_init done, calling ramfs_mount...\n");
        if (ramfs_mount("/") == 0) {
            serial_print("[BOOT] ramfs_mount done, creating directories...\n");
            vfs_mkdir("/system");
            vfs_mkdir("/system/dbg");
            vfs_mkdir("/system/dbg/up");
            vfs_mkdir("/system/dbg/up/out");
            vfs_mkdir("/system/internet");
            vfs_mkdir("/system/firmware");
            serial_print("[BOOT] directories created, extracting modules...\n");

            /* Extract Limine modules into RAMFS so VFS can find them */
            extern int limine_get_module_count(void);
            extern void *limine_get_module(int index, size_t *size_out, const char **path_out);

            int mod_count = limine_get_module_count();
            for (int i = 0; i < mod_count; i++) {
                size_t mod_size;
                const char *mod_path;
                void *mod_data = limine_get_module(i, &mod_size, &mod_path);
                if (!mod_data || !mod_path) continue;

                /* Skip ELF binaries (knl.bin, init, shell, desk) - they don't need to be in RAMFS */
                const char *base = strrchr(mod_path, '/');
                const char *fname = base ? base + 1 : mod_path;
                if (strcmp(fname, "knl.bin") == 0 ||
                    strcmp(fname, "init") == 0 ||
                    strcmp(fname, "shell") == 0 ||
                    strcmp(fname, "desk") == 0) {
                    continue;
                }

                /* Copy module data to RAMFS via VFS */
                struct vfs_file *f = vfs_open(mod_path, VFS_MODE_WRITE | VFS_MODE_CREATE);
                if (f) {
                    vfs_write(f, mod_data, mod_size);
                    vfs_close(f);
                    serial_print("[BOOT] Extracted Limine module to RAMFS: ");
                    serial_print(mod_path);
                    serial_print(" (");
                    /* Print size */
                    char sbuf[16];
                    int sp = 0;
                    uint64_t tmp = mod_size;
                    if (tmp == 0) sbuf[sp++] = '0';
                    else {
                        char tbuf[16]; int t = 0;
                        while (tmp > 0) { tbuf[t++] = '0' + (tmp % 10); tmp /= 10; }
                        while (t > 0) sbuf[sp++] = tbuf[--t];
                    }
                    sbuf[sp] = '\0';
                    serial_print(sbuf);
                    serial_print(" bytes)\n");
                } else {
                    serial_print("[BOOT] WARNING: Failed to extract to RAMFS: ");
                    serial_print(mod_path);
                    serial_print("\n");
                }
            }

            /* 创建标准目录 */
            vfs_mkdir("/root");
            vfs_mkdir("/usr");
            vfs_mkdir("/usr/bin");
            vfs_mkdir("/desktop");
            vfs_mkdir("/desktop/root");
            vfs_mkdir("/desktop/user");
            vfs_mkdir("/ini");
            vfs_mkdir("/ini/usrroot");
            vfs_mkdir("/prgm");
            vfs_mkdir("/prgm/sys");
            vfs_mkdir("/prgm/windowprogram");
            vfs_mkdir("/zrr64");
            vfs_mkdir("/zrr64/zdonk");
            vfs_mkdir("/zrr64/zdonk/AI");

            /* 加载用户配置 */
            struct config_file *user_cfg = config_load("/ini/usrroot/user.cfg");
            uint32_t cfg_uid = 1000;
            uint32_t cfg_gid = 1000;
            char cfg_username[32] = "user";
            if (user_cfg != NULL) {
                cfg_uid = (uint32_t)config_get_int(user_cfg, "uid", 1000);
                cfg_gid = (uint32_t)config_get_int(user_cfg, "gid", 1000);
                const char *uname = config_get_string(user_cfg, "name", "user");
                int ulen = 0;
                while (uname[ulen] && ulen < 31) { cfg_username[ulen] = uname[ulen]; ulen++; }
                cfg_username[ulen] = 0;
                config_free(user_cfg);
                serial_print("[BOOT] Loaded user config: uid=");
                char ubuf[16]; int up = 0;
                uint32_t uv = cfg_uid;
                if (uv == 0) ubuf[up++] = '0';
                else { char ut[16]; int utp = 0; while (uv > 0) { ut[utp++] = '0' + (uv % 10); uv /= 10; } while (utp > 0) ubuf[up++] = ut[--utp]; }
                ubuf[up] = '\n'; ubuf[up+1] = '\0';
                serial_print(ubuf);
            } else {
                serial_print("[BOOT] No user config, using defaults\n");
            }

            /* Store username globally */
            {
                extern char g_boot_cfg_username[32];
                for (int i = 0; i < 32; i++) { g_boot_cfg_username[i] = cfg_username[i]; if (!cfg_username[i]) break; }
            }

            /* 设置目录所有权和权限 */
            vfs_chown("/root", 0, 0);
            vfs_chmod("/root", 0700);
            vfs_chown("/usr", 0, 0);
            vfs_chmod("/usr", 0755);
            vfs_chown("/usr/bin", 0, 0);
            vfs_chmod("/usr/bin", 0755);
            vfs_chown("/desktop", 0, 0);
            vfs_chmod("/desktop", 0755);
            vfs_chown("/desktop/user", cfg_uid, cfg_gid);
            vfs_chmod("/desktop/user", 0755);

            /* Create /usr/[username] directory for user */
            {
                char usr_dir[64] = "/usr/";
                int pl = 5;
                const char *u = cfg_username;
                while (*u && pl < 62) usr_dir[pl++] = *u++;
                usr_dir[pl] = 0;
                vfs_mkdir(usr_dir);
                vfs_chown(usr_dir, cfg_uid, cfg_gid);
                vfs_chmod(usr_dir, 0755);
            }

            /* ========== 自主文件系统：生成默认文件 ========== */
            ramfs_populate_defaults(cfg_uid, cfg_gid, cfg_username);

            /* 保存配置供后续使用 */
            extern uint32_t g_boot_cfg_uid;
            extern uint32_t g_boot_cfg_gid;
            g_boot_cfg_uid = cfg_uid;
            g_boot_cfg_gid = cfg_gid;
        }
    } else {
        /* FAT32 初始化成功 - 但仍需确保所有必需文件存在 */
        serial_print("[BOOT] FAT32 filesystem ready, ensuring required files exist...\n");

        /* 加载用户配置 */
        struct config_file *user_cfg = config_load("/ini/usrroot/user.cfg");
        uint32_t cfg_uid = 1000;
        uint32_t cfg_gid = 1000;
        char cfg_username[32] = "user";
        if (user_cfg != NULL) {
            cfg_uid = (uint32_t)config_get_int(user_cfg, "uid", 1000);
            cfg_gid = (uint32_t)config_get_int(user_cfg, "gid", 1000);
            const char *uname = config_get_string(user_cfg, "name", "user");
            int ulen = 0;
            while (uname[ulen] && ulen < 31) { cfg_username[ulen] = uname[ulen]; ulen++; }
            cfg_username[ulen] = 0;
            config_free(user_cfg);
            serial_print("[BOOT] Loaded user config from FAT32\n");
        } else {
            serial_print("[BOOT] No user.cfg on FAT32, will create default\n");
        }

        /* Store username globally */
        {
            extern char g_boot_cfg_username[32];
            for (int i = 0; i < 32; i++) { g_boot_cfg_username[i] = cfg_username[i]; if (!cfg_username[i]) break; }
        }

        /* Ensure all required directories exist on FAT32 */
        vfs_mkdir("/root");
        vfs_mkdir("/usr");
        vfs_mkdir("/usr/bin");
        vfs_mkdir("/desktop");
        vfs_mkdir("/desktop/root");
        vfs_mkdir("/desktop/user");
        vfs_mkdir("/ini");
        vfs_mkdir("/ini/usrroot");
        vfs_mkdir("/prgm");
        vfs_mkdir("/prgm/sys");
        vfs_mkdir("/prgm/windowprogram");
        vfs_mkdir("/zrr64");
        vfs_mkdir("/zrr64/zdonk");
        vfs_mkdir("/zrr64/zdonk/AI");
        vfs_mkdir("/system");
        vfs_mkdir("/system/internet");

        /* Create /usr/[username] directory */
        {
            char usr_dir[64] = "/usr/";
            int pl = 5;
            const char *u = cfg_username;
            while (*u && pl < 62) usr_dir[pl++] = *u++;
            usr_dir[pl] = 0;
            vfs_mkdir(usr_dir);
        }

        vfs_chown("/root", 0, 0);
        vfs_chmod("/root", 0700);
        vfs_chown("/usr", 0, 0);
        vfs_chmod("/usr", 0755);
        vfs_chown("/usr/bin", 0, 0);
        vfs_chmod("/usr/bin", 0755);
        vfs_chown("/desktop", 0, 0);
        vfs_chmod("/desktop", 0755);
        vfs_chown("/desktop/user", cfg_uid, cfg_gid);
        vfs_chmod("/desktop/user", 0755);

        /* ========== 关键：无论FAT32是否存在，都确保默认文件存在 ========== */
        ramfs_populate_defaults(cfg_uid, cfg_gid, cfg_username);

        g_boot_cfg_uid = cfg_uid;
        g_boot_cfg_gid = cfg_gid;

        serial_print("[BOOT] FAT32 + AUTOFS complete\n");
    }

    ext4_init();

    /* ========== 阶段 4: 进程管理 ========== */
    task_init();
    scheduler_init();
    scheduler_init_cpu(0);  /* 为 CPU 0 创建 idle 线程 */

    /* 启动所有应用处理器 (AP) */
    smp_start_aps();
    /* 为每个 AP 初始化调度器 */
    for (uint32_t i = 1; i < smp_cpu_count; i++) {
        scheduler_init_cpu(i);
    }

    /* ========== 阶段 4.5: 双内核系统 ========== */
    /* 初始化双内核系统（如果配置了 primary=ztknl） */
    if (primary_role == 1) {
        serial_print("[BOOT] Initializing dual kernel system (Ztknl primary)...\n");
        int ret = dual_kernel_init(1, "/system/aknl.bin");
        if (ret == 0) {
            serial_print("[BOOT] Dual kernel initialized, starting secondary kernel...\n");
            dual_kernel_start_secondary();
        } else {
            serial_print("[BOOT] WARNING: Dual kernel init failed, continuing single kernel mode\n");
        }
    } else if (primary_role == 2) {
        /* Azknl 为主内核，Ztknl 作为协处理器运行 */
        serial_print("[BOOT] Initializing dual kernel system (Azknl primary)...\n");
        dual_kernel_init(0, NULL);
    } else {
        /* 未知角色，单内核模式，跳过核间通信 */
        serial_print("[BOOT] Single kernel mode (no inter-kernel)\n");
        /* inter_kernel_init()  skipped - no dual kernel configured */
    }

    ipc_init();

    /* ========== 阶段 4.5b: 加载 Rsknl (Rust 监控内核) ========== */
    {
        extern int rustknl_load(void);
        serial_print("[BOOT] Loading Rsknl (Rust monitoring kernel)...\n");
        int rsknl_ret = rustknl_load();
        if (rsknl_ret == 0) {
            serial_print("[BOOT] Rsknl loaded and running at protected address\n");
        } else {
            serial_print("[BOOT] WARNING: Rsknl load failed (code=");
            char ebuf[16]; int ep = 0;
            int ev = rsknl_ret;
            if (ev < 0) { ebuf[ep++] = '-'; ev = -ev; }
            if (ev == 0) ebuf[ep++] = '0';
            else { char et[16]; int etp = 0; while (ev > 0) { et[etp++] = '0' + (ev % 10); ev /= 10; } while (etp > 0) ebuf[ep++] = et[--etp]; }
            ebuf[ep] = '\n'; ebuf[ep+1] = '\0';
            serial_print(ebuf);
            serial_print("[BOOT] Continuing without Rsknl monitoring\n");
        }
    }

    syscall_init();
    utx_init();
    pmem_global_init(0, 64 * 1024 * 1024, PMEM_TYPE_FILE_BACKED, NULL);
    zpipe_global_init(4 * 1024 * 1024);
    cap_init();
    sh_global_init();
    co_init();
    het_global_init();
    opu_init();
    nzc_global_init();
    rt_global_init();
    pa_global_init();
    ktest_global_init();
    rlimit_global_init();
    pf_prevention_init();
    klog_flush_to_file();
    apic_start_timer(1000);  /* 1000Hz = 1 tick/ms */

    /* ========== 阶段 4.6: 文件系统 RPC 代理 ========== */
    /* 如果双内核已初始化，尝试使用 Azknl 的文件系统 */
    if (inter_kernel_is_peer_ready()) {
        serial_print("[FS] Initializing filesystem RPC proxy...\n");
        if (fs_proxy_init() == 0) {
            /* 尝试挂载 RPC 文件系统到根目录 */
            if (vfs_mount("/", "azknl-rpc", fs_proxy_get_ops(), NULL) == 0) {
                serial_print("[FS] RPC filesystem mounted at /\n");
            } else {
                serial_print("[FS] WARNING: RPC filesystem mount failed, using local FS\n");
            }
        } else {
            serial_print("[FS] WARNING: RPC proxy init failed, using local FS\n");
        }
    }

    /* ========== 阶段 5: 兼容层初始化 ========== */
    abi_init();
    loader_init();
    exec_init();
    pe_init();
    dll_init();
    subkernel_init();

    /* ========== 阶段 6: 网络子系统 ========== */
    net_init();
    socket_init();

    /* 有线网络 (e1000) */
    e1000_init();  /* 初始化 e1000 网卡 */

    /* 无线网络 (WiFi) */
    wifi_init();
    extern int rtl8188eu_driver_init(void);
    rtl8188eu_driver_init();  /* 注册 RTL8188EU USB WiFi 驱动 */

    /* 网络连接 (DHCP 自动获取 - 有线优先，然后 WiFi) */
    extern int tcp_init(void);
    extern int net_autoconnect(void);
    extern int wifi_autoconnect(void);
    tcp_init();

    /* 先尝试有线网络 */
    if (net_autoconnect() < 0) {
        /* 有线失败，尝试 WiFi */
        wifi_autoconnect();
    }

    /* 网络对时 (NTP)，失败则用 RTC/BIOS 时间 */
    extern void time_sync_ntp(void);
    time_sync_ntp();

    /* ========== 阶段 7: 图形子系统 ========== */
    fb_init();
    image_init();

    /* ========== 阶段 8: 高级功能 ========== */
    hotupdate_init();
    zts_init();  /* 初始化 ZTS7 动态算法系统 */
    ai_subsys_init();  /* 初始化 AI 自开发子系统 */

    /* ========== 阶段 8.5: 启动诊断报告 ========== */
    /* Skip diag report - may cause stack/memory corruption leading to triple fault */

    serial_print("[BOOT] Skipping diag report, launching programs...\n");

    /* ========== 阶段 9: 启动完成 ========== */
    serial_print("[BOOT] Stage 9: Boot complete, launching programs\n");
    /* Skip fb_clear - the desktop will overwrite it anyway */
    serial_print("[BOOT] Launching programs...\n");
    launch_programs();

    /* 启用中断并开始调度 */
    serial_print("[BOOT] Enabling interrupts...\n");
    cpu_enable_interrupts();
    serial_print("[BOOT] Starting scheduler...\n");
    scheduler_start();
    serial_print("[BOOT] ERROR: scheduler_start returned!\n");

    /* 永不返回 */
    while (1) {
        cpu_halt();
    }
}

/* ========== 内核入口 ========== */

/* _start 函数禁用栈保护，因为在 GDT 和 SMP 初始化之前 gs 段基址未设置
 * 编译器 -fstack-protector-strong 会在函数开头插入读取 gs:0x28 的代码
 * 如果 gs 未正确设置，会导致段错误和 triple fault 重启
 */
__attribute__((no_stack_protector))
__attribute__((force_align_arg_pointer))
void _start(void) {
    /* Initialize serial immediately for debug output */
    serial_init();
    serial_print("[BOOT] _start() entered\n");

    /* ========== 关键: 必须在调用任何启用栈保护的函数之前设置 GS_BASE ==========
     * GCC -fstack-protector-strong 会在函数 prologue 中插入:
     *   mov rax, qword ptr gs:[0x28]
     * 如果 GS_BASE 未设置 (实机 firmware 退出后通常为 0), 会访问线性地址 0x28
     * 该地址未映射 => #PF, 但 IDT 还未安装 => triple fault => 重启
     * QEMU 不崩溃是因为 OVMF/SeaBIOS 残留状态, 实机会立即崩溃!
     */
    {
        extern struct cpu_local cpu_data[];
        uint64_t tsc_lo, tsc_hi;
        asm volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
        uint64_t tsc = (tsc_hi << 32) | tsc_lo;
        uint64_t canary = tsc ^ (tsc >> 32) ^ 0xDEADBEEFCAFEBABEULL;

        cpu_data[0].cpu_id = 0;
        cpu_data[0].apic_id = 0;
        cpu_data[0].state = CPU_STATE_RUNNING;
        cpu_data[0].stack_canary = canary;

        uint64_t gs_base = (uint64_t)&cpu_data[0];
        asm volatile("wrmsr" : :
            "c"((uint32_t)0xC0000101),  /* IA32_GS_BASE */
            "a"((uint32_t)gs_base),
            "d"((uint32_t)(gs_base >> 32)));
        serial_print("[BOOT] GS_BASE set early\n");
    }

    /* 安全检查 Limine 响应 */
    if (fb_request.response == NULL) {
        serial_print("[BOOT] FATAL: fb_request.response is NULL on this machine\n");
        goto skip_framebuffer;
    }
    serial_print("[BOOT] fb_request.response OK\n");
    
    if (fb_request.response->framebuffer_count == 0) {
        serial_print("[BOOT] FATAL: framebuffer_count is 0\n");
        goto skip_framebuffer;
    }
    
    if (fb_request.response->framebuffers[0] == NULL) {
        serial_print("[BOOT] FATAL: framebuffers[0] is NULL\n");
        goto skip_framebuffer;
    }

    /* 不填充背景色，保持黑色直到 logo 显示 */

skip_framebuffer:
    /* 初始化帧缓冲（如果可用）或使用默认控制台 */
    console_init_with_fb(&fb_request);

    /* 注释: 暂时不禁用屏幕输出, 以便观察内核启动信息 (实机调试用) */
    /* console_disable_output(); */
    
    /* 屏幕上输出 boot 标识 (需要在 console_init_with_fb 之后) */
    screen_ok("Kernel _start entered");
    screen_ok("Console initialized");
    screen_info("Entering kernel_main()...");

    /* 调用内核主函数 */
    kernel_main();

    /* 不应该到达这里 */
    kernel_panic("kernel_main returned");
}

/*
 * flush_framebuffer_cache - 刷新用户空间 framebuffer 缓存
 * 
 * 在调度器切换任务之前调用，确保用户空间对 framebuffer 的写入
 * 被刷新到物理内存。这是解决用户空间映射 framebuffer 缓存一致性
 * 问题的关键函数。
 *
 * 实现策略：使用 wbinvd 指令写回并刷新整个 CPU 缓存
 * 注意：这是一个重量级操作，但在 QEMU 环境中是必要的
 */
void flush_framebuffer_cache(void) {
    /* 执行 wbinvd - Writeback and Invalidate Cache */
    asm volatile("wbinvd" ::: "memory");
}
