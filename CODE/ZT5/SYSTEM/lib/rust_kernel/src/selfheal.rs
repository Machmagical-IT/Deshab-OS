//! Self-Healing System for ZratroOS
//!
//! A comprehensive self-healing framework providing automatic fault detection,
//! diagnosis, and recovery capabilities for the Rust monitoring kernel.
//! Supports: fault detection, automatic repair, kernel rollback, and escalation.

use crate::serial::SerialWriter;
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};

pub use crate::selfheal_types::*;

/// Global fault counter for generating unique fault IDs
static FAULT_COUNTER: AtomicU64 = AtomicU64::new(1);

/// Flag indicating if recovery is in progress
static RECOVERY_IN_PROGRESS: AtomicBool = AtomicBool::new(false);

/// Maximum rollback snapshots
const MAX_SNAPSHOTS: usize = 4;

/// Kernel state snapshot for rollback
#[derive(Clone, Copy)]
pub struct KernelSnapshot {
    pub timestamp: u64,
    pub tick_counter: u64,
    pub task_count: u64,
    pub free_pages: u64,
    pub heap_used: u64,
    pub health_score: u8, // 0-100, 100 = perfect
}

/// Rollback manager - stores snapshots and handles rollback requests
pub struct RollbackManager {
    snapshots: [KernelSnapshot; MAX_SNAPSHOTS],
    snapshot_count: usize,
    current_idx: usize,
    rollback_available: bool,
}

impl RollbackManager {
    pub const fn new() -> Self {
        RollbackManager {
            snapshots: [KernelSnapshot {
                timestamp: 0,
                tick_counter: 0,
                task_count: 0,
                free_pages: 0,
                heap_used: 0,
                health_score: 0,
            }; MAX_SNAPSHOTS],
            snapshot_count: 0,
            current_idx: 0,
            rollback_available: false,
        }
    }

    /// Save a snapshot of current kernel state
    pub fn save_snapshot(&mut self, tick: u64, tasks: u64, free_pages: u64, heap: u64, health: u8) {
        let snap = KernelSnapshot {
            timestamp: Self::read_tsc(),
            tick_counter: tick,
            task_count: tasks,
            free_pages: free_pages,
            heap_used: heap,
            health_score: health,
        };

        self.snapshots[self.current_idx] = snap;
        self.current_idx = (self.current_idx + 1) % MAX_SNAPSHOTS;
        if self.snapshot_count < MAX_SNAPSHOTS {
            self.snapshot_count += 1;
        }

        // Only allow rollback if we have a healthy snapshot
        self.rollback_available = self.has_healthy_snapshot();
    }

    /// Check if we have a healthy snapshot to roll back to
    fn has_healthy_snapshot(&self) -> bool {
        for i in 0..self.snapshot_count {
            if self.snapshots[i].health_score >= 80 {
                return true;
            }
        }
        false
    }

    /// Get the best snapshot for rollback (highest health score)
    pub fn best_rollback_snapshot(&self) -> Option<&KernelSnapshot> {
        if self.snapshot_count == 0 {
            return None;
        }

        let mut best: Option<&KernelSnapshot> = None;
        let mut best_score: u8 = 0;

        for i in 0..self.snapshot_count {
            if self.snapshots[i].health_score > best_score {
                best_score = self.snapshots[i].health_score;
                best = Some(&self.snapshots[i]);
            }
        }

        // Only return if health score is reasonable
        if best_score >= 60 {
            best
        } else {
            None
        }
    }

    /// Check if rollback is available
    pub fn is_rollback_available(&self) -> bool {
        self.rollback_available
    }

    /// Read TSC for timestamp
    fn read_tsc() -> u64 {
        unsafe {
            let mut tsc: u64 = 0;
            core::arch::asm!("rdtsc", out("rax") tsc, options(nomem, nostack));
            tsc
        }
    }
}

/// The main Self-Healing Manager
pub struct SelfHealManager {
    config: HealConfig,
    stats: HealStats,
    last_fault_time: u64,
    rollback: RollbackManager,
    consecutive_faults: u64,
    max_consecutive_faults: u64,
    escalation_level: u8, // 0=normal, 1=elevated, 2=critical, 3=emergency
}

impl SelfHealManager {
    /// Create a new self-healing manager with default configuration
    pub const fn new() -> Self {
        SelfHealManager {
            config: HealConfig::default(),
            stats: HealStats::new(),
            last_fault_time: 0,
            rollback: RollbackManager::new(),
            consecutive_faults: 0,
            max_consecutive_faults: 10,
            escalation_level: 0,
        }
    }

    /// Initialize the self-healing system
    pub fn init(&self) -> HealResult<()> {
        Ok(())
    }

    /// Report a fault to the self-healing manager
    pub fn report_fault(&mut self, fault: FaultContext, serial: &mut SerialWriter) -> HealResult<RecoveryAction> {
        let fault_id = FAULT_COUNTER.fetch_add(1, Ordering::SeqCst);
        self.last_fault_time = Self::read_tsc();
        self.stats.record_fault(fault.fault_type);
        self.consecutive_faults += 1;

        // Log the fault
        self.log_fault(serial, fault_id, &fault);

        // Escalate if too many consecutive faults
        if self.consecutive_faults >= self.max_consecutive_faults {
            self.escalation_level = (self.escalation_level + 1).min(3);
            serial.print_str("[SELFHEAL] Escalation level: ");
            serial.print_char(b'0' + self.escalation_level);
            serial.print_str("\n");
        }

        // Determine recovery action based on severity and escalation
        let action = self.determine_recovery_action(&fault);

        // Execute recovery if severity is high enough
        if fault.severity >= self.config.min_severity_for_recovery {
            RECOVERY_IN_PROGRESS.store(true, Ordering::SeqCst);
            if let Err(_) = self.execute_recovery(&fault, action, serial) {
                self.stats.record_failure();
                RECOVERY_IN_PROGRESS.store(false, Ordering::SeqCst);
                return Err(HealError::RecoveryFailed);
            }
            self.stats.record_success();
            RECOVERY_IN_PROGRESS.store(false, Ordering::SeqCst);
        }

        Ok(action)
    }

    /// Save a kernel snapshot for potential rollback
    pub fn save_snapshot(&mut self, tick: u64, tasks: u64, free_pages: u64, heap: u64, health: u8) {
        self.rollback.save_snapshot(tick, tasks, free_pages, heap, health);
    }

    /// Check if rollback is available
    pub fn is_rollback_available(&self) -> bool {
        self.rollback.is_rollback_available()
    }

    /// Reset consecutive fault counter (call when system is healthy)
    pub fn reset_fault_counter(&mut self) {
        if self.consecutive_faults > 0 {
            self.consecutive_faults = 0;
            if self.escalation_level > 0 {
                self.escalation_level -= 1;
            }
        }
    }

    /// Log a fault to the serial output
    fn log_fault(&self, serial: &mut SerialWriter, fault_id: u64, fault: &FaultContext) {
        serial.print_str("\n[SELFFAULT #");
        Self::print_u64_static(serial, fault_id);
        serial.print_str("] ");

        let type_str = match fault.fault_type {
            FaultType::OutOfMemory => "OutOfMemory",
            FaultType::MemoryLeak => "MemoryLeak",
            FaultType::Deadlock => "Deadlock",
            FaultType::NullPointer => "NullPointer",
            FaultType::DivideByZero => "DivideByZero",
            FaultType::PageFault => "PageFault",
            FaultType::KernelPanic => "KernelPanic",
            FaultType::HardwareFault => "HardwareFault",
            FaultType::WatchdogTimeout => "WatchdogTimeout",
            FaultType::StackOverflow => "StackOverflow",
            FaultType::BufferOverflow => "BufferOverflow",
            FaultType::UseAfterFree => "UseAfterFree",
            FaultType::DoubleFault => "DoubleFault",
            FaultType::TripleFault => "TripleFault",
            FaultType::GeneralProtection => "GeneralProtection",
            FaultType::InvalidOpcode => "InvalidOpcode",
            FaultType::RaceCondition => "RaceCondition",
            FaultType::Livelock => "Livelock",
            _ => "Unknown",
        };
        serial.print_str(type_str);

        serial.print_str(" severity=");
        let sev_str = match fault.severity {
            Severity::Info => "Info",
            Severity::Minor => "Minor",
            Severity::Moderate => "Moderate",
            Severity::Major => "Major",
            Severity::Critical => "Critical",
            Severity::Fatal => "Fatal",
        };
        serial.print_str(sev_str);
        serial.print_str(" escalation=");
        serial.print_char(b'0' + self.escalation_level);
        serial.print_str("\n");
    }

    /// Determine the appropriate recovery action for a fault
    fn determine_recovery_action(&self, fault: &FaultContext) -> RecoveryAction {
        // Escalation affects recovery decisions
        match self.escalation_level {
            0 => match fault.severity {
                Severity::Info | Severity::Minor => RecoveryAction::None,
                Severity::Moderate => RecoveryAction::Retry,
                Severity::Major => RecoveryAction::Restart,
                Severity::Critical => RecoveryAction::Isolate,
                Severity::Fatal => RecoveryAction::Reboot,
            },
            1 => match fault.severity {
                Severity::Info | Severity::Minor => RecoveryAction::Retry,
                Severity::Moderate => RecoveryAction::Restart,
                Severity::Major => RecoveryAction::Isolate,
                Severity::Critical | Severity::Fatal => RecoveryAction::Rollback,
            },
            2 => match fault.severity {
                Severity::Info | Severity::Minor => RecoveryAction::Restart,
                Severity::Moderate | Severity::Major => RecoveryAction::Rollback,
                Severity::Critical | Severity::Fatal => RecoveryAction::Reboot,
            },
            _ => RecoveryAction::Reboot, // Emergency level
        }
    }

    /// Execute a recovery action
    fn execute_recovery(&mut self, fault: &FaultContext, action: RecoveryAction, serial: &mut SerialWriter) -> HealResult<()> {
        serial.print_str("[SELFHEAL] Executing: ");
        let action_str = match action {
            RecoveryAction::None => "None",
            RecoveryAction::Retry => "Retry",
            RecoveryAction::Restart => "Restart",
            RecoveryAction::Isolate => "Isolate",
            RecoveryAction::Rollback => "Rollback",
            RecoveryAction::Hotfix => "Hotfix",
            RecoveryAction::Failover => "Failover",
            RecoveryAction::Reboot => "Reboot",
            RecoveryAction::Escalate => "Escalate",
            _ => "Unknown",
        };
        serial.print_str(action_str);
        serial.print_str("\n");

        match action {
            RecoveryAction::None => {
                // No action needed
                Ok(())
            }
            RecoveryAction::Retry => {
                // Wait a bit and signal Ztknl to retry
                Self::delay(1000);
                serial.print_str("[SELFHEAL] Retry signaled to Ztknl\n");
                Ok(())
            }
            RecoveryAction::Restart => {
                // Signal Ztknl to restart the affected component
                serial.print_str("[SELFHEAL] Component restart requested\n");
                Ok(())
            }
            RecoveryAction::Isolate => {
                // Isolate the faulty component
                serial.print_str("[SELFHEAL] Fault isolation requested\n");
                Ok(())
            }
            RecoveryAction::Rollback => {
                // Attempt rollback to last healthy snapshot
                if self.rollback.is_rollback_available() {
                    if let Some(snap) = self.rollback.best_rollback_snapshot() {
                        serial.print_str("[SELFHEAL] Rolling back to snapshot: tick=");
                        Self::print_u64_static(serial, snap.tick_counter);
                        serial.print_str(" health=");
                        serial.print_char(b'0' + (snap.health_score / 10));
                        serial.print_char(b'0' + (snap.health_score % 10));
                        serial.print_str("%\n");
                        // Signal Ztknl to perform rollback
                        return Ok(());
                    }
                }
                serial.print_str("[SELFHEAL] No healthy snapshot for rollback, escalating\n");
                self.escalation_level = (self.escalation_level + 1).min(3);
                Err(HealError::NoHandler)
            }
            RecoveryAction::Reboot => {
                serial.print_str("[SELFHEAL] System reboot required!\n");
                // In a real implementation, trigger a system reboot
                // For safety, we just log and continue monitoring
                Ok(())
            }
            _ => {
                serial.print_str("[SELFHEAL] Unknown recovery action\n");
                Err(HealError::InvalidState)
            }
        }
    }

    /// Read TSC for timestamp
    fn read_tsc() -> u64 {
        unsafe {
            let mut tsc: u64 = 0;
            core::arch::asm!("rdtsc", out("rax") tsc, options(nomem, nostack));
            tsc
        }
    }

    /// Simple delay loop
    fn delay(cycles: u64) {
        for _ in 0..cycles {
            unsafe { core::arch::asm!("nop", options(nomem, nostack)); }
        }
    }

    /// Get current statistics
    pub fn stats(&self) -> &HealStats {
        &self.stats
    }

    /// Check if recovery is in progress
    pub fn is_recovery_in_progress() -> bool {
        RECOVERY_IN_PROGRESS.load(Ordering::SeqCst)
    }

    /// Static helper to print u64
    fn print_u64_static(serial: &mut SerialWriter, mut n: u64) {
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
