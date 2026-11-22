/*
 * boot_diag.c - 启动诊断和自检模块
 *
 * 诊断项目:
 * 1. 双内核通信自检
 * 2. FS RPC 功能测试
 * 3. 磁盘文件系统检查
 * 4. 完整的启动诊断报告
 */

#include "console.h"
#include "string.h"
#include "inter_kernel.h"
#include "fs_proxy.h"
#include "vfs.h"
#include "disk.h"
#include "cpu.h"

/* 诊断结果结构 */
struct diag_result {
    const char *test_name;
    int passed;
    const char *detail;
};

/* 诊断报告 */
struct boot_diag_report {
    int total_tests;
    int passed_tests;
    int failed_tests;
    struct diag_result results[32];
    int result_count;
};

static struct boot_diag_report diag_report;

/* 串口输出辅助函数 */
extern void serial_print(const char *str);
extern void print_dec(int64_t);
extern void println(const char *);
extern void print_hex(uint64_t);

/* 添加诊断结果 */
static void diag_add_result(const char *name, int passed, const char *detail) {
    if (diag_report.result_count >= 32) return;

    diag_report.results[diag_report.result_count].test_name = name;
    diag_report.results[diag_report.result_count].passed = passed;
    diag_report.results[diag_report.result_count].detail = detail;
    diag_report.result_count++;
    diag_report.total_tests++;

    if (passed) {
        diag_report.passed_tests++;
        serial_print("  [通过] ");
    } else {
        diag_report.failed_tests++;
        serial_print("  [失败] ");
    }
    serial_print(name);
    if (detail) {
        serial_print(" - ");
        serial_print(detail);
    }
    serial_print("\n");
}

/* ========== 1. 双内核通信自检 ========== */

static void diag_dual_kernel_check(void) {
    serial_print("\n=== 双内核通信检查 ===\n");

    /* 检查共享内存 */
    uint64_t base, size;
    inter_kernel_get_shm_info(&base, &size);

    if (base == INTER_KERNEL_SHM_PHYS && size == INTER_KERNEL_SHM_SIZE) {
        diag_add_result("共享内存映射", 1, "正常 (0x1000000, 64KB)");
    } else {
        diag_add_result("共享内存映射", 0, "映射无效");
        serial_print("  [信息] 单内核模式,跳过双内核检查\n");
        return;
    }

    /* 安全检查: 确保共享内存地址已映射 */
    extern int is_address_mapped(uint64_t addr);
    if (!is_address_mapped(base)) {
        diag_add_result("共享内存访问", 0, "地址未映射 (单内核模式)");
        serial_print("  [信息] 共享内存未映射,无法访问\n");
        return;
    }

    /* 检查共享内存魔数 */
    struct inter_kernel_shm *shm = (struct inter_kernel_shm *)(uint64_t)base;
    if (shm->header.magic == INTER_KERNEL_MAGIC) {
        diag_add_result("共享内存魔数", 1, "0x5A4A414E (ZJAN)");
    } else {
        diag_add_result("共享内存魔数", 0, "魔数无效");
        return;
    }

    /* 检查对方内核状态 */
    if (inter_kernel_is_peer_ready()) {
        diag_add_result("对端内核状态", 1, "就绪");

        /* 检查协议版本 */
        if (shm->header.version == INTER_KERNEL_VERSION) {
            diag_add_result("协议版本", 1, "v1");
        } else {
            diag_add_result("协议版本", 0, "版本不匹配");
        }

        /* 检查 CPU 信息 */
        serial_print("  [信息] Ztknl 处理器数: ");
        print_dec(shm->header.ztknl_cpu_count);
        serial_print(", Azknl 处理器数: ");
        print_dec(shm->header.azknl_cpu_count);
        serial_print("\n");

        /* 尝试发送测试消息 */
        uint64_t test_data = 0x12345678;
        int ret = inter_kernel_send_msg(MSG_TYPE_PING, &test_data, 1);
        if (ret == 0) {
            diag_add_result("消息发送测试", 1, "发送成功");
        } else {
            diag_add_result("消息发送测试", 0, "发送失败");
        }
    } else {
        diag_add_result("对端内核状态", 0, "未就绪");
        serial_print("  [信息] Azknl 未运行或无响应\n");
    }
}

/* ========== 2. FS RPC 功能测试 ========== */

static void diag_fs_rpc_check(void) {
    serial_print("\n=== 文件系统 RPC 检查 ===\n");

    /* 检查 FS Proxy 初始化状态 */
    if (!fs_proxy_available()) {
        diag_add_result("FS代理可用性", 0, "不可用");
        serial_print("  [信息] FS RPC 需要双内核模式下运行 Azknl\n");
        return;
    }

    diag_add_result("FS代理可用性", 1, "可用");

    /* 测试 1: stat 根目录 */
    struct vfs_stat stat;
    int ret = fs_proxy_get_ops()->stat("/", &stat);
    if (ret == VFS_OK) {
        diag_add_result("RPC: stat(/)", 1, "成功");
    } else {
        diag_add_result("RPC: stat(/)", 0, "失败");
    }

    /* 测试 2: 创建测试目录 */
    ret = fs_proxy_get_ops()->mkdir("/test_diag");
    if (ret == VFS_OK) {
        diag_add_result("RPC: mkdir(/test_diag)", 1, "成功");
    } else {
        diag_add_result("RPC: mkdir(/test_diag)", 0, "失败");
    }

    /* 测试 3: 打开/创建测试文件 */
    struct vfs_file file;
    memset(&file, 0, sizeof(file));
    ret = fs_proxy_get_ops()->open(&file, "/test_diag/hello.txt",
                                    VFS_MODE_WRITE | VFS_MODE_CREATE);
    if (ret == VFS_OK) {
        diag_add_result("RPC: open(创建)", 1, "成功");

        /* 测试 4: 写入数据 */
        const char *test_data = "你好来自Ztknl的RPC!";
        ssize_t written = fs_proxy_get_ops()->write(&file, test_data, strlen(test_data));
        if (written > 0) {
            diag_add_result("RPC: write", 1, "写入成功");
        } else {
            diag_add_result("RPC: write", 0, "写入失败");
        }

        /* 关闭文件 */
        fs_proxy_get_ops()->close(&file);
    } else {
        diag_add_result("RPC: open(创建)", 0, "失败");
    }

    /* 测试 5: 读取测试文件 */
    memset(&file, 0, sizeof(file));
    ret = fs_proxy_get_ops()->open(&file, "/test_diag/hello.txt", VFS_MODE_READ);
    if (ret == VFS_OK) {
        char buf[64];
        memset(buf, 0, sizeof(buf));
        ssize_t n = fs_proxy_get_ops()->read(&file, buf, sizeof(buf) - 1);
        if (n > 0) {
            diag_add_result("RPC: read", 1, buf);
        } else {
            diag_add_result("RPC: read", 0, "读取失败");
        }
        fs_proxy_get_ops()->close(&file);
    } else {
        diag_add_result("RPC: open(读取)", 0, "失败");
    }

    /* 测试 6: seek 测试 */
    memset(&file, 0, sizeof(file));
    ret = fs_proxy_get_ops()->open(&file, "/test_diag/hello.txt", VFS_MODE_READ);
    if (ret == VFS_OK) {
        ret = fs_proxy_get_ops()->seek(&file, 5);
        if (ret == VFS_OK && file.offset == 5) {
            diag_add_result("RPC: seek", 1, "偏移=5");
        } else {
            diag_add_result("RPC: seek", 0, "seek失败");
        }
        fs_proxy_get_ops()->close(&file);
    }

    /* 测试 7: 清理测试文件 */
    fs_proxy_get_ops()->unlink("/test_diag/hello.txt");
    fs_proxy_get_ops()->rmdir("/test_diag");
}

/* ========== 3. 磁盘文件系统检查 ========== */

static void diag_disk_fs_check(void) {
    serial_print("\n=== 磁盘文件系统检查 ===\n");

    /* 检查磁盘设备 */
    int num_disks = disk_get_device_count();

    if (num_disks > 0) {
        serial_print("  [信息] 发现 ");
        print_dec(num_disks);
        serial_print(" 个磁盘设备\n");

        for (int i = 0; i < num_disks; i++) {
            struct disk_device *dev = disk_get_device(i);
            if (dev && dev->initialized) {
                serial_print("  [信息] 磁盘 ");
                print_dec(i);
                serial_print(": ");
                serial_print(dev->name);
                serial_print(" 大小=");
                print_dec(dev->sectors * dev->sector_size / (1024 * 1024));
                serial_print("MB, 类型=");
                print_dec(dev->type);
                serial_print("\n");
            }
        }
        diag_add_result("磁盘检测", 1, "发现设备");
    } else {
        diag_add_result("磁盘检测", 0, "无磁盘设备");
        serial_print("  [信息] 运行在 RAMFS 模式 (无物理磁盘)\n");
    }

    /* 检查 VFS 状态 */
    struct vfs_mount *mount = NULL;
    extern struct vfs_mount *vfs_get_mounts(void);
    mount = vfs_get_mounts();

    int mount_count = 0;
    while (mount) {
        serial_print("  [信息] 挂载: ");
        serial_print(mount->path);
        serial_print(" -> ");
        serial_print(mount->device);
        serial_print("\n");
        mount = mount->next;
        mount_count++;
    }

    if (mount_count > 0) {
        diag_add_result("VFS挂载", 1, "挂载活跃");
    } else {
        diag_add_result("VFS挂载", 0, "无挂载");
    }

    /* 测试本地 VFS 操作 */
    struct vfs_stat stat;
    int ret = vfs_stat("/", &stat);
    if (ret == VFS_OK) {
        diag_add_result("本地VFS: stat(/)", 1, "成功");
    } else {
        diag_add_result("本地VFS: stat(/)", 0, "失败");
    }

    /* 检查关键目录 */
    const char *paths[] = {"/system", "/prgm", "/prgm/sys"};
    for (int i = 0; i < 3; i++) {
        ret = vfs_stat(paths[i], &stat);
        if (ret == VFS_OK) {
            diag_add_result("路径存在", 1, paths[i]);
        } else {
            diag_add_result("路径存在", 0, paths[i]);
        }
    }
}

/* ========== 4. 打印诊断报告 ========== */

void boot_print_diag_report(void) {
    memset(&diag_report, 0, sizeof(diag_report));

    serial_print("\n");
    serial_print("========================================\n");
    serial_print("  ZratroOS 启动诊断报告\n");
    serial_print("========================================\n");

    /* 执行所有检查 */
    diag_dual_kernel_check();
    diag_fs_rpc_check();
    diag_disk_fs_check();

    /* 打印总结 */
    serial_print("\n========================================\n");
    serial_print("  摘要\n");
    serial_print("========================================\n");
    serial_print("  总测试数:  ");
    print_dec(diag_report.total_tests);
    serial_print("\n");
    serial_print("  通过:      ");
    print_dec(diag_report.passed_tests);
    serial_print("\n");
    serial_print("  失败:      ");
    print_dec(diag_report.failed_tests);
    serial_print("\n");

    if (diag_report.failed_tests == 0) {
        serial_print("\n  [OK] 所有诊断测试通过!\n");
    } else {
        serial_print("\n  [警告] 部分测试失败。请检查上述详情。\n");
    }
    serial_print("========================================\n\n");
}

/* 获取诊断结果 */
int boot_diag_get_result(void) {
    return (diag_report.failed_tests == 0) ? 1 : 0;
}
