//! Self-Healing Types - Fault types, recovery actions, and healing structures

/// Fault types supported by the self-healing system
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FaultType {
    Unknown = 0,
    OutOfMemory = 1,
    MemoryLeak = 2,
    StackOverflow = 3,
    NullPointer = 4,
    DivideByZero = 5,
    PageFault = 6,
    GeneralProtection = 7,
    InvalidOpcode = 8,
    DoubleFault = 9,
    TripleFault = 10,
    Deadlock = 11,
    Livelock = 12,
    RaceCondition = 13,
    BufferOverflow = 14,
    UseAfterFree = 15,
    HardwareFault = 16,
    WatchdogTimeout = 17,
    KernelPanic = 18,
}

/// Severity levels for faults
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u8)]
pub enum Severity {
    Info = 0,
    Minor = 1,
    Moderate = 2,
    Major = 3,
    Critical = 4,
    Fatal = 5,
}

impl Severity {
    pub const fn is_recoverable(self) -> bool {
        matches!(self, Self::Info | Self::Minor | Self::Moderate | Self::Major | Self::Critical)
    }

    pub const fn requires_immediate_action(self) -> bool {
        matches!(self, Self::Critical | Self::Fatal)
    }
}

/// Recovery actions
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum RecoveryAction {
    None = 0,
    Retry = 1,
    Restart = 2,
    Isolate = 3,
    Rollback = 4,
    Hotfix = 5,
    Failover = 6,
    Reboot = 7,
    Escalate = 8,
}

/// Fault context information
#[derive(Debug, Clone)]
pub struct FaultContext {
    pub fault_type: FaultType,
    pub severity: Severity,
    pub component: [u8; 32],
    pub message: [u8; 64],
}

impl FaultContext {
    pub const fn new(fault_type: FaultType, severity: Severity) -> Self {
        FaultContext {
            fault_type,
            severity,
            component: [0; 32],
            message: [0; 64],
        }
    }

    pub fn set_component(&mut self, name: &str) {
        let bytes = name.as_bytes();
        let len = bytes.len().min(31);
        self.component[..len].copy_from_slice(&bytes[..len]);
        self.component[len] = 0;
    }

    pub fn set_message(&mut self, msg: &str) {
        let bytes = msg.as_bytes();
        let len = bytes.len().min(63);
        self.message[..len].copy_from_slice(&bytes[..len]);
        self.message[len] = 0;
    }
}

/// Statistics for healing operations
#[derive(Debug, Clone, Default)]
pub struct HealStats {
    pub total_faults: u64,
    pub successful_recoveries: u64,
    pub failed_recoveries: u64,
}

impl HealStats {
    pub const fn new() -> Self {
        HealStats {
            total_faults: 0,
            successful_recoveries: 0,
            failed_recoveries: 0,
        }
    }

    pub fn record_fault(&mut self, fault_type: FaultType) {
        self.total_faults += 1;
    }

    pub fn record_success(&mut self) {
        self.successful_recoveries += 1;
    }

    pub fn record_failure(&mut self) {
        self.failed_recoveries += 1;
    }
}

/// Configuration for self-healing system
#[derive(Debug, Clone, Copy)]
pub struct HealConfig {
    pub min_severity_for_recovery: Severity,
    pub max_concurrent_recoveries: u32,
}

impl HealConfig {
    pub const fn default() -> Self {
        HealConfig {
            min_severity_for_recovery: Severity::Minor,
            max_concurrent_recoveries: 3,
        }
    }
}

/// Error types for healing operations
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HealError {
    None = 0,
    InvalidState = 1,
    NoHandler = 2,
    OutOfResources = 3,
    ComponentNotFound = 4,
    RecoveryFailed = 5,
    Timeout = 6,
}

pub type HealResult<T> = Result<T, HealError>;

/// Recovery policy trait
pub trait RecoveryPolicy {
    fn execute(&self, fault: &FaultContext) -> HealResult<RecoveryAction>;
}

/// Default recovery policy
pub struct DefaultRecoveryPolicy;

impl DefaultRecoveryPolicy {
    pub const fn new() -> Self {
        DefaultRecoveryPolicy
    }
}

impl RecoveryPolicy for DefaultRecoveryPolicy {
    fn execute(&self, fault: &FaultContext) -> HealResult<RecoveryAction> {
        match fault.severity {
            Severity::Info | Severity::Minor => Ok(RecoveryAction::None),
            Severity::Moderate => Ok(RecoveryAction::Retry),
            Severity::Major => Ok(RecoveryAction::Restart),
            Severity::Critical => Ok(RecoveryAction::Isolate),
            Severity::Fatal => Ok(RecoveryAction::Reboot),
        }
    }
}

/// Serial output control - minimal output for release builds
pub struct OutputControl {
    pub verbose: bool,
    pub level: OutputLevel,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OutputLevel {
    Silent = 0,    // No output except panics
    Error = 1,     // Errors only
    Warn = 2,      // Warnings and errors
    Info = 3,      // Normal operational output
    Debug = 4,     // Verbose debugging
}

impl OutputControl {
    pub const fn new(level: OutputLevel) -> Self {
        OutputControl {
            verbose: matches!(level, OutputLevel::Debug),
            level,
        }
    }

    pub const fn should_print(&self, min_level: OutputLevel) -> bool {
        self.level as u8 >= min_level as u8
    }
}
