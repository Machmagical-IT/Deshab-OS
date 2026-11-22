//! Kernel Health Monitor - Real-time kernel state monitoring
//!
//! Monitors Ztknl kernel health through shared memory regions,
//! detecting anomalies, memory corruption, and system hangs.

use core::sync::atomic::{AtomicU64, AtomicBool, Ordering};

/// Shared memory layout for Ztknl -> Rsknl communication
/// This structure must match the C kernel's definition
#[repr(C)]
pub struct KernelHealthBlock {
    /// Magic number: 0x5A544B4E ("ZTKN")
    pub magic: u64,
    /// Kernel tick counter (incremented each scheduler tick)
    pub tick_counter: u64,
    /// Number of running tasks
    pub task_count: u64,
    /// Number of page faults since boot
    pub page_fault_count: u64,
    /// Free physical pages
    pub free_pages: u64,
    /// Total physical pages
    pub total_pages: u64,
    /// Kernel heap used (bytes)
    pub heap_used: u64,
    /// Kernel heap total (bytes)
    pub heap_total: u64,
    /// Last panic message offset (0 = no panic)
    pub panic_offset: u64,
    /// Checksum of above fields
    pub checksum: u64,
    /// Self-heal command from Rsknl -> Ztknl
    pub heal_command: u64,
    /// Rollback request flag
    pub rollback_flag: u64,
    /// Reserved
    pub reserved: [u64; 4],
}

/// Health check result
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HealthStatus {
    Healthy,
    Degraded,
    Unhealthy,
    Critical,
    Unknown,
}

/// Kernel snapshot for comparison
#[derive(Clone, Copy)]
pub struct KernelSnapshot {
    pub tick_counter: u64,
    pub task_count: u64,
    pub free_pages: u64,
    pub heap_used: u64,
    pub page_fault_count: u64,
}

/// Kernel health monitor
pub struct HealthMonitor {
    health_block_addr: u64,
    last_snapshot: KernelSnapshot,
    stale_tick_count: u64,
    consecutive_stale: u64,
    max_stale_ticks: u64,
    last_health: HealthStatus,
    check_count: u64,
    anomaly_count: u64,
}

impl HealthMonitor {
    pub const fn new(health_block_addr: u64) -> Self {
        HealthMonitor {
            health_block_addr,
            last_snapshot: KernelSnapshot {
                tick_counter: 0,
                task_count: 0,
                free_pages: 0,
                heap_used: 0,
                page_fault_count: 0,
            },
            stale_tick_count: 0,
            consecutive_stale: 0,
            max_stale_ticks: 100, // ~10 seconds at 10Hz
            last_health: HealthStatus::Unknown,
            check_count: 0,
            anomaly_count: 0,
        }
    }

    /// Read the kernel health block from shared memory
    fn read_health_block(&self) -> Option<KernelHealthBlock> {
        if self.health_block_addr == 0 {
            return None;
        }
        unsafe {
            let ptr = self.health_block_addr as *const KernelHealthBlock;
            // Validate pointer is readable
            let magic = core::ptr::read_volatile(&(*ptr).magic);
            if magic != 0x5A544B4E {
                return None;
            }
            Some(core::ptr::read_volatile(ptr))
        }
    }

    /// Verify checksum of health block
    fn verify_checksum(block: &KernelHealthBlock) -> bool {
        let mut sum: u64 = 0;
        sum = sum.wrapping_add(block.magic);
        sum = sum.wrapping_add(block.tick_counter);
        sum = sum.wrapping_add(block.task_count);
        sum = sum.wrapping_add(block.page_fault_count);
        sum = sum.wrapping_add(block.free_pages);
        sum = sum.wrapping_add(block.total_pages);
        sum = sum.wrapping_add(block.heap_used);
        sum = sum.wrapping_add(block.heap_total);
        sum = sum.wrapping_add(block.panic_offset);
        sum == block.checksum
    }

    /// Run a health check cycle
    pub fn check(&mut self) -> HealthStatus {
        self.check_count += 1;

        let block = match self.read_health_block() {
            Some(b) => b,
            None => {
                self.anomaly_count += 1;
                self.last_health = HealthStatus::Unknown;
                return HealthStatus::Unknown;
            }
        };

        // Verify checksum
        if !Self::verify_checksum(&block) {
            self.anomaly_count += 1;
            self.last_health = HealthStatus::Critical;
            return HealthStatus::Critical;
        }

        // Check for kernel hang (tick counter not advancing)
        if block.tick_counter == self.last_snapshot.tick_counter {
            self.consecutive_stale += 1;
            if self.consecutive_stale >= self.max_stale_ticks {
                self.anomaly_count += 1;
                self.last_health = HealthStatus::Critical;
                return HealthStatus::Critical;
            } else if self.consecutive_stale >= self.max_stale_ticks / 2 {
                self.last_health = HealthStatus::Unhealthy;
                return HealthStatus::Unhealthy;
            }
        } else {
            self.consecutive_stale = 0;
        }

        // Check memory pressure
        let mem_pressure = if block.total_pages > 0 {
            100 - (block.free_pages * 100 / block.total_pages)
        } else {
            100
        };

        // Check heap pressure
        let heap_pressure = if block.heap_total > 0 {
            block.heap_used * 100 / block.heap_total
        } else {
            0
        };

        // Check for panic
        if block.panic_offset != 0 {
            self.anomaly_count += 1;
            self.last_health = HealthStatus::Critical;
            return HealthStatus::Critical;
        }

        // Determine overall health
        let health = if mem_pressure > 95 || heap_pressure > 95 {
            HealthStatus::Critical
        } else if mem_pressure > 85 || heap_pressure > 85 {
            HealthStatus::Unhealthy
        } else if mem_pressure > 70 || heap_pressure > 70 {
            HealthStatus::Degraded
        } else {
            HealthStatus::Healthy
        };

        // Update snapshot
        self.last_snapshot = KernelSnapshot {
            tick_counter: block.tick_counter,
            task_count: block.task_count,
            free_pages: block.free_pages,
            heap_used: block.heap_used,
            page_fault_count: block.page_fault_count,
        };

        self.last_health = health;
        health
    }

    /// Send a heal command to Ztknl through shared memory
    pub fn send_heal_command(&mut self, command: u64) {
        if self.health_block_addr == 0 {
            return;
        }
        unsafe {
            let ptr = self.health_block_addr as *mut KernelHealthBlock;
            core::ptr::write_volatile(&mut (*ptr).heal_command, command);
        }
    }

    /// Request a kernel rollback
    pub fn request_rollback(&mut self) {
        if self.health_block_addr == 0 {
            return;
        }
        unsafe {
            let ptr = self.health_block_addr as *mut KernelHealthBlock;
            core::ptr::write_volatile(&mut (*ptr).rollback_flag, 1);
        }
    }

    /// Get current health status
    pub fn current_health(&self) -> HealthStatus {
        self.last_health
    }

    /// Get anomaly count
    pub fn anomaly_count(&self) -> u64 {
        self.anomaly_count
    }

    /// Get last snapshot
    pub fn last_snapshot(&self) -> &KernelSnapshot {
        &self.last_snapshot
    }
}

/// Self-test routines for Rsknl
pub struct SelfTest {
    passed: u64,
    failed: u64,
}

impl SelfTest {
    pub const fn new() -> Self {
        SelfTest { passed: 0, failed: 0 }
    }

    /// Run all self-tests
    pub fn run_all(&mut self, serial: &mut crate::serial::SerialWriter) -> bool {
        serial.print_str("[RUSTKERNEL] === Running Self-Tests ===\n");

        self.test_memory(serial);
        self.test_checksum(serial);
        self.test_atomic(serial);
        self.test_stack(serial);
        self.test_idt_gdt(serial);

        serial.print_str("[RUSTKERNEL] Self-tests: passed=");
        self.print_u64(serial, self.passed);
        serial.print_str(" failed=");
        self.print_u64(serial, self.failed);
        serial.print_str("\n");

        self.failed == 0
    }

    /// Test memory read/write
    fn test_memory(&mut self, serial: &mut crate::serial::SerialWriter) {
        serial.print_str("[RUSTKERNEL] Test: memory read/write... ");
        // Test stack memory
        let mut test_buf = [0u64; 16];
        for i in 0..16 {
            test_buf[i] = (i as u64) * 0x12345678_9ABCDEF0;
        }
        let mut ok = true;
        for i in 0..16 {
            if test_buf[i] != (i as u64) * 0x12345678_9ABCDEF0 {
                ok = false;
                break;
            }
        }
        if ok {
            self.passed += 1;
            serial.print_str("OK\n");
        } else {
            self.failed += 1;
            serial.print_str("FAIL\n");
        }
    }

    /// Test checksum calculation
    fn test_checksum(&mut self, serial: &mut crate::serial::SerialWriter) {
        serial.print_str("[RUSTKERNEL] Test: checksum... ");
        let data = [0x41424344_45464748u64, 0x494A4B4C_4D4E4F50];
        let mut sum: u64 = 0;
        for &v in &data {
            sum = sum.wrapping_add(v);
        }
        let expected: u64 = 0x8A8E8F90_92949598;
        if sum == expected {
            self.passed += 1;
            serial.print_str("OK\n");
        } else {
            self.failed += 1;
            serial.print_str("FAIL\n");
        }
    }

    /// Test atomic operations
    fn test_atomic(&mut self, serial: &mut crate::serial::SerialWriter) {
        serial.print_str("[RUSTKERNEL] Test: atomic operations... ");
        let val = AtomicU64::new(42);
        let old = val.swap(100, Ordering::SeqCst);
        let current = val.load(Ordering::SeqCst);
        if old == 42 && current == 100 {
            self.passed += 1;
            serial.print_str("OK\n");
        } else {
            self.failed += 1;
            serial.print_str("FAIL\n");
        }
    }

    /// Test stack integrity
    fn test_stack(&mut self, serial: &mut crate::serial::SerialWriter) {
        serial.print_str("[RUSTKERNEL] Test: stack integrity... ");
        // Recursive test to verify stack works correctly
        fn recurse(n: u64) -> u64 {
            if n == 0 { 1 } else { n + recurse(n - 1) }
        }
        let result = recurse(100);
        if result == 5050 {
            self.passed += 1;
            serial.print_str("OK\n");
        } else {
            self.failed += 1;
            serial.print_str("FAIL\n");
        }
    }

    /// Test IDT and GDT are loaded
    fn test_idt_gdt(&mut self, serial: &mut crate::serial::SerialWriter) {
        serial.print_str("[RUSTKERNEL] Test: GDT/IDT loaded... ");
        let mut gdt_ok = false;
        let mut idt_ok = false;
        unsafe {
            // Read GDTR
            let mut gdtr: [u8; 10] = [0; 10];
            core::arch::asm!(
                "sgdt [{}]",
                in(reg) gdtr.as_mut_ptr(),
                options(nostack)
            );
            let limit = u16::from_le_bytes([gdtr[0], gdtr[1]]);
            gdt_ok = limit > 0;

            // Read IDTR
            let mut idtr: [u8; 10] = [0; 10];
            core::arch::asm!(
                "sidt [{}]",
                in(reg) idtr.as_mut_ptr(),
                options(nostack)
            );
            let ilimit = u16::from_le_bytes([idtr[0], idtr[1]]);
            idt_ok = ilimit > 0;
        }
        if gdt_ok && idt_ok {
            self.passed += 1;
            serial.print_str("OK\n");
        } else {
            self.failed += 1;
            serial.print_str("FAIL\n");
        }
    }

    fn print_u64(&self, serial: &mut crate::serial::SerialWriter, mut n: u64) {
        if n == 0 {
            serial.print_char(b'0');
            return;
        }
        let mut buf = [0u8; 20];
        let mut i = 19;
        while n > 0 {
            buf[i] = b'0' + (n % 10) as u8;
            n /= 10;
            i -= 1;
        }
        for j in (i + 1)..20 {
            serial.print_char(buf[j]);
        }
    }
}
