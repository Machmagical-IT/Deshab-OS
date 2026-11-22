//! Shared Memory Monitoring

pub struct SharedMemory {
    base: *mut u8,
    size: usize,
    checksum: u64,
}

impl SharedMemory {
    pub fn new(base: *mut u8, size: usize) -> Self {
        let mut shmem = SharedMemory {
            base,
            size,
            checksum: 0,
        };
        shmem.checksum = shmem.calculate_checksum();
        shmem
    }

    pub fn verify(&self) -> bool {
        self.calculate_checksum() == self.checksum
    }

    pub fn update_checksum(&mut self) {
        self.checksum = self.calculate_checksum();
    }

    fn calculate_checksum(&self) -> u64 {
        let mut sum: u64 = 0;
        unsafe {
            for i in (0..self.size).step_by(8) {
                let remaining = self.size - i;
                if remaining >= 8 {
                    let val = (self.base.add(i) as *const u64).read_volatile();
                    sum = sum.wrapping_add(val);
                }
            }
        }
        sum
    }
}
