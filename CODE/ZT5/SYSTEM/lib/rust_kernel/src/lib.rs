//! ZratroOS Rust Self-Healing Kernel (Rsknl)
//!
//! A self-healing monitoring kernel that runs alongside Ztknl (main C kernel),
//! providing fault detection, automatic recovery, system resilience,
//! health monitoring, self-testing, and rollback capabilities.
//!
//! Rsknl runs at a special protected memory address that no other program
//! can access, ensuring it remains operational even when the main kernel
//! encounters issues.

#![no_std]

mod gdt;
mod idt;
mod serial;
mod vga;
mod watchdog;
mod shared_mem;

// Self-healing modules
mod selfheal_types;
mod selfheal;

// Health monitoring module
mod health_monitor;

use serial::SerialWriter;
use vga::VGAWriter;
use watchdog::Watchdog;
use shared_mem::SharedMemory;
use selfheal::SelfHealManager;
use health_monitor::{HealthMonitor, SelfTest};

/// Rsknl protected memory region base address
/// This is at 0xFFFFFFFF_C0000000, above the main kernel
const RSKNL_BASE: u64 = 0xFFFFFFFF_C0000000;

/// Kernel initialization and monitoring state
struct KernelState {
    serial: SerialWriter,
    vga: VGAWriter,
    watchdog: Option<Watchdog>,
    shared_mem: Option<SharedMemory>,
    self_heal: Option<SelfHealManager>,
    health_monitor: Option<HealthMonitor>,
    self_test: SelfTest,
    tick_count: u64,
}

impl KernelState {
    fn new() -> Self {
        KernelState {
            serial: SerialWriter::new(),
            vga: VGAWriter::new(),
            watchdog: None,
            shared_mem: None,
            self_heal: None,
            health_monitor: None,
            self_test: SelfTest::new(),
            tick_count: 0,
        }
    }

    fn kprint(&mut self, s: &str) {
        self.vga.print_str(s);
        self.serial.print_str(s);
    }

    fn kprintln(&mut self) {
        self.vga.print_char(b'\n');
        self.serial.print_char('\n' as u8);
    }

    fn print_hex(&mut self, n: u64) {
        const HEX: &[u8] = b"0123456789abcdef";
        self.kprint("0x");
        for i in (0..16).rev() {
            let digit = (n >> (i * 4)) & 0xF;
            self.vga.print_char(HEX[digit as usize]);
            self.serial.print_char(HEX[digit as usize]);
        }
    }

    fn print_dec(&mut self, mut n: u64) {
        if n == 0 {
            self.kprint("0");
            return;
        }
        let mut buf = [0u8; 20];
        let mut i = 19;
        while n > 0 {
            buf[i] = b'0' + (n % 10) as u8;
            n /= 10;
            i -= 1;
        }
        self.vga.print_str(core::str::from_utf8(&buf[i + 1..]).unwrap_or("??"));
        self.serial.print_str(core::str::from_utf8(&buf[i + 1..]).unwrap_or("??"));
    }
}

/// Main entry point called from Ztknl
#[no_mangle]
pub extern "C" fn rustknl_entry(
    magic: u64,
    ztknl_addr: u64,
    ztknl_size: u64,
    shared_mem_addr: u64,
    shared_mem_size: u64,
) -> ! {
    // Validate magic number
    if magic != 0x52555354_4B4E4C00 {
        loop { unsafe { core::arch::asm!("hlt"); } }
    }

    let mut state = KernelState::new();

    state.kprint("[RSKNL] === ZratroOS Rust Monitoring Kernel v0.3 ===");
    state.kprintln();
    state.kprint("[RSKNL] Protected base: ");
    state.print_hex(RSKNL_BASE);
    state.kprintln();

    // Initialize VGA and serial
    state.kprint("[RSKNL] VGA driver initialized");
    state.kprintln();
    state.kprint("[RSKNL] Serial driver initialized (COM1 115200bps)");
    state.kprintln();

    // Initialize GDT
    state.kprint("[RSKNL] Initializing GDT... ");
    unsafe { gdt::init_gdt(); }
    state.kprint("OK");
    state.kprintln();

    // Initialize IDT
    state.kprint("[RSKNL] Initializing IDT... ");
    unsafe { idt::init_idt(); }
    state.kprint("OK");
    state.kprintln();

    // Run self-tests
    state.kprint("[RSKNL] Running self-tests...");
    state.kprintln();
    let self_test_ok = state.self_test.run_all(&mut state.serial);
    if !self_test_ok {
        state.kprint("[RSKNL] WARNING: Self-tests failed! Operating in degraded mode.");
        state.kprintln();
    } else {
        state.kprint("[RSKNL] All self-tests passed.");
        state.kprintln();
    }

    state.kprint("[RSKNL] Ztknl: addr=");
    state.print_hex(ztknl_addr);
    state.kprint(" size=");
    state.print_dec(ztknl_size);
    state.kprint(" bytes");
    state.kprintln();

    state.kprint("[RSKNL] Shared mem: addr=");
    state.print_hex(shared_mem_addr);
    state.kprint(" size=");
    state.print_dec(shared_mem_size);
    state.kprintln();

    // Initialize shared memory monitoring
    if shared_mem_addr != 0 && shared_mem_size > 0 {
        state.kprint("[RSKNL] Initializing shared memory monitor... ");
        let shmem = SharedMemory::new(shared_mem_addr as *mut u8, shared_mem_size as usize);
        if shmem.verify() {
            state.kprint("OK (checksum verified)");
        } else {
            state.kprint("WARN (checksum mismatch on first read)");
        }
        state.kprintln();
        state.shared_mem = Some(shmem);
    }

    // Initialize watchdog
    state.kprint("[RSKNL] Initializing watchdog... ");
    let wd = Watchdog::new(ztknl_addr, ztknl_size);
    state.kprint("OK");
    state.kprintln();
    state.watchdog = Some(wd);

    // Initialize health monitor
    state.kprint("[RSKNL] Initializing health monitor... ");
    let hm = HealthMonitor::new(shared_mem_addr);
    state.kprint("OK");
    state.kprintln();
    state.health_monitor = Some(hm);

    // Initialize self-healing system
    state.kprint("[RSKNL] Initializing self-healing system... ");
    let heal_mgr = SelfHealManager::new();
    state.kprint("OK (fault types: 19, recovery actions: 9, rollback: enabled)");
    state.kprintln();
    state.self_heal = Some(heal_mgr);

    state.kprint("[RSKNL] === Monitoring loop started ===");
    state.kprintln();
    state.kprint("[RSKNL] Rsknl is running at protected address, no program can access this memory.");
    state.kprintln();

    // Main monitoring loop
    loop {
        state.tick_count += 1;

        // Run watchdog check
        if let Some(ref mut wd) = state.watchdog {
            wd.run_once();
            if !wd.is_ztknl_healthy() {
                state.kprint("[RSKNL] ALERT: Ztknl health check failed!");
                state.kprintln();

                // Report fault to self-healing system
                if let Some(ref mut heal) = state.self_heal {
                    let mut fault = selfheal_types::FaultContext::new(
                        selfheal_types::FaultType::WatchdogTimeout,
                        selfheal_types::Severity::Critical,
                    );
                    fault.set_component("ztknl");
                    fault.set_message("Kernel health check failed");
                    let _ = heal.report_fault(fault, &mut state.serial);
                }
            }
        }

        // Run health monitor check
        let mut health_msg: Option<&str> = None;
        let mut rollback_requested = false;
        if let Some(ref mut hm) = state.health_monitor {
            let health = hm.check();
            if health != health_monitor::HealthStatus::Healthy {
                let health_str = match health {
                    health_monitor::HealthStatus::Degraded => "Degraded",
                    health_monitor::HealthStatus::Unhealthy => "Unhealthy",
                    health_monitor::HealthStatus::Critical => "CRITICAL",
                    health_monitor::HealthStatus::Unknown => "Unknown",
                    _ => "?",
                };

                if state.tick_count % 100 == 0 {
                    health_msg = Some(health_str);
                }

                // Auto-heal on critical status
                if health == health_monitor::HealthStatus::Critical {
                    if let Some(ref mut heal) = state.self_heal {
                        let mut fault = selfheal_types::FaultContext::new(
                            selfheal_types::FaultType::KernelPanic,
                            selfheal_types::Severity::Critical,
                        );
                        fault.set_component("ztknl");
                        fault.set_message("Critical health status detected");
                        let _ = heal.report_fault(fault, &mut state.serial);

                        // Request rollback if available
                        if heal.is_rollback_available() {
                            rollback_requested = true;
                            hm.request_rollback();
                        }
                    }
                }
            } else {
                // System is healthy - reset fault counter and save snapshot
                if let Some(ref mut heal) = state.self_heal {
                    heal.reset_fault_counter();
                    let snap = hm.last_snapshot();
                    let health_score = 95u8; // Healthy
                    heal.save_snapshot(
                        snap.tick_counter,
                        snap.task_count,
                        snap.free_pages,
                        snap.heap_used,
                        health_score,
                    );
                }
            }
        }

        // Print health messages outside the borrow
        if let Some(hs) = health_msg {
            state.kprint("[RSKNL] Health: ");
            state.kprint(hs);
            state.kprintln();
        }
        if rollback_requested {
            state.kprint("[RSKNL] Requesting kernel rollback...");
            state.kprintln();
        }

        // Check shared memory
        let shmem_needs_update = if let Some(ref shmem) = state.shared_mem {
            !shmem.verify()
        } else {
            false
        };

        if shmem_needs_update {
            state.kprint("[RSKNL] WARN: Shared memory checksum mismatch!");
            state.kprintln();
            if let Some(ref mut shmem) = state.shared_mem {
                shmem.update_checksum();
            }

            // Report as a fault
            if let Some(ref mut heal) = state.self_heal {
                let mut fault = selfheal_types::FaultContext::new(
                    selfheal_types::FaultType::MemoryLeak,
                    selfheal_types::Severity::Moderate,
                );
                fault.set_component("shared_mem");
                fault.set_message("Checksum mismatch detected");
                let _ = heal.report_fault(fault, &mut state.serial);
            }
        }

        // Periodic self-test (every ~10000 ticks)
        if state.tick_count % 10000 == 0 {
            state.kprint("[RSKNL] Periodic self-test...");
            state.kprintln();
            let ok = state.self_test.run_all(&mut state.serial);
            if !ok {
                state.kprint("[RSKNL] Periodic self-test FAILED!");
                state.kprintln();
            }
        }

        // Periodic status report (every ~1000 ticks)
        if state.tick_count % 1000 == 0 {
            let status_data = state.health_monitor.as_ref().map(|hm| {
                let snap = hm.last_snapshot();
                (snap.tick_counter, snap.task_count, snap.free_pages, snap.heap_used, hm.anomaly_count())
            });
            if let Some((tick, tasks, free, heap, anomalies)) = status_data {
                state.kprint("[RSKNL] Status: tick=");
                state.print_dec(tick);
                state.kprint(" tasks=");
                state.print_dec(tasks);
                state.kprint(" free_pages=");
                state.print_dec(free);
                state.kprint(" heap=");
                state.print_dec(heap);
                state.kprint(" anomalies=");
                state.print_dec(anomalies);
                state.kprintln();
            }
        }

        unsafe { core::arch::asm!("hlt"); }
    }
}

/// Default entry point (for standalone boot)
#[no_mangle]
pub extern "C" fn _start() -> ! {
    rustknl_entry(0x52555354_4B4E4C00, 0, 0, 0, 0)
}

/// Panic handler with self-healing support
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    // Write panic message directly to serial port using inline assembly
    unsafe {
        let serial_port: u16 = 0x3F8;
        let msg = b"[PANIC] Rsknl kernel panic!\n";

        for c in msg {
            // Wait for transmitter holding register empty (bit 5 of LSR)
            loop {
                let status: u8;
                core::arch::asm!(
                    "in al, dx",
                    out("al") status,
                    in("dx") serial_port + 5,
                    options(nomem, nostack)
                );
                if status & 0x20 != 0 { break; }
            }

            // Write character
            core::arch::asm!(
                "out dx, al",
                in("al") *c,
                in("dx") serial_port,
                options(nomem, nostack)
            );
        }
    }

    // Halt the system
    loop { unsafe { core::arch::asm!("hlt"); } }
}
