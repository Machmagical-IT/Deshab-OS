//! Watchdog - Monitors Ztknl and provides crash recovery

pub struct Watchdog {
    ztknl_addr: u64,
    ztknl_size: u64,
    ztknl_healthy: bool,
    tick_count: u64,
}

impl Watchdog {
    pub const fn new(
        ztknl_addr: u64,
        ztknl_size: u64,
    ) -> Self {
        Watchdog {
            ztknl_addr,
            ztknl_size,
            ztknl_healthy: true,
            tick_count: 0,
        }
    }

    /// Run one iteration of the monitoring loop (non-blocking)
    pub fn run_once(&mut self) {
        self.tick_count += 1;
        self.check_ztknl();
    }

    fn check_ztknl(&mut self) {
        if self.ztknl_addr != 0 {
            unsafe {
                let ptr = self.ztknl_addr as *const u8;
                let _ = ptr.read_volatile();
                let ptr_end = (self.ztknl_addr + self.ztknl_size - 1) as *const u8;
                let _ = ptr_end.read_volatile();
            }
            self.ztknl_healthy = true;
        }
    }

    pub fn is_ztknl_healthy(&self) -> bool {
        self.ztknl_healthy
    }
}
