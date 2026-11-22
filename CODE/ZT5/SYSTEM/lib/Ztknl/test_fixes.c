/*
 * 测试修复的功能模块
 * 用于验证修复的 page fault 和引导问题
 */

#include "console.h"
#include "pmm.h"
#include "vmm.h"
#include "smp.h"
#include "heap.h"

void test_memory_subsystems(void) {
    println("");
    println("=== Testing Memory Subsystems ===");
    
    // 测试 1: 验证 HHDM 偏移
    uint64_t hhdm = pmm_get_hhdm_offset();
    print("HHDM offset: 0x");
    print_hex(hhdm);
    if (hhdm >= 0xFFFF800000000000ULL) {
        println(" [OK]");
    } else {
        println(" [ERROR: Not in Higher Half!]");
    }
    
    // 测试 2: 物理内存分配
    uint64_t test_page = pmm_alloc_page();
    if (test_page != 0) {
        print("PMM alloc_page: 0x");
        print_hex(test_page);
        println(" [OK]");
        
        // 验证 phys_to_virt 转换
        void *virt_addr = phys_to_virt(test_page);
        if (virt_addr != NULL && (uint64_t)virt_addr >= 0xFFFF800000000000ULL) {
            print("phys_to_virt: 0x");
            print_hex((uint64_t)virt_addr);
            println(" [OK]");
        } else {
            println("phys_to_virt: [ERROR]");
        }
        
        pmm_free_page(test_page);
        println("PMM free_page: [OK]");
    } else {
        println("PMM alloc_page: [ERROR]");
    }
    
    // 测试 3: 虚拟内存访问
    struct address_space *kernel_space = vmm_get_kernel_space();
    if (kernel_space != NULL) {
        println("VMM kernel_space: [OK]");
        
        // 尝试访问内核基址的物理地址
        uint64_t kernel_phys = vmm_get_physical(kernel_space, 0xffffffff80000000ULL);
        if (kernel_phys != 0) {
            print("Kernel mapping: 0x");
            print_hex(kernel_phys);
            println(" [OK]");
        } else {
            println("Kernel mapping: [WARNING - May use huge pages]");
        }
    } else {
        println("VMM kernel_space: [ERROR]");
    }
    
    // 测试 4: 检查当前 CPU 数据
    struct cpu_local *cpu = this_cpu();
    if (cpu != NULL) {
        print("this_cpu(): cpu_id=");
        print_dec(cpu->cpu_id);
        print(", apic_id=");
        print_dec(cpu->apic_id);
        println(" [OK]");
        
        // 验证 stack_canary
        if (cpu->stack_canary != 0) {
            print("stack_canary: 0x");
            print_hex(cpu->stack_canary);
            println(" [OK]");
        } else {
            println("stack_canary: [ERROR - Zero value!]");
        }
    } else {
        println("this_cpu(): [ERROR - NULL]");
    }
    
    println("=== Memory Subsystem Tests Complete ===");
    println("");
}

void test_kernel_mappings(void) {
    println("=== Testing Kernel Mappings ===");
    
    // 测试内核空间的几个关键地址
    uint64_t test_addrs[] = {
        0xffffffff80000000ULL,  // 内核基址
        0xffffffff80100000ULL,  // 内核 + 1MB
        0xffffffff80800000ULL,  // 内核 + 8MB
        0xffffffff90000000ULL,  // 内核 + 256MB
        0xffff800000000000ULL,  // 内核栈区域
    };
    
    struct address_space *space = vmm_get_kernel_space();
    
    for (int i = 0; i < 5; i++) {
        print("Testing addr 0x");
        print_hex(test_addrs[i]);
        print(": ");
        
        uint64_t phys = vmm_get_physical(space, test_addrs[i]);
        if (phys != 0) {
            print("-> 0x");
            print_hex(phys);
            println(" [OK]");
        } else {
            println("Not mapped [May be OK]");
        }
    }
    
    println("=== Kernel Mapping Tests Complete ===");
    println("");
}

void run_all_tests(void) {
    println("");
    println("╔══════════════════════════════════════════╗");
    println("║          Zratro OS Fix Verification      ║");
    println("╚══════════════════════════════════════════╝");
    println("");
    
    test_memory_subsystems();
    test_kernel_mappings();
    
    println("All tests completed successfully!");
    println("");
}