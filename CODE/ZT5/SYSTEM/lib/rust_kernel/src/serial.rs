//! Serial Port Driver (COM1 0x3F8)

const COM1_PORT: u16 = 0x3F8;

pub struct SerialWriter;

impl SerialWriter {
    pub fn new() -> Self {
        unsafe {
            // Disable interrupts
            outb(COM1_PORT + 1, 0x00);
            // Enable DLAB
            outb(COM1_PORT + 3, 0x80);
            // Set baud rate (115200)
            outb(COM1_PORT + 0, 0x01);
            outb(COM1_PORT + 1, 0x00);
            // 8 bits, no parity, one stop bit
            outb(COM1_PORT + 3, 0x03);
            // Enable FIFO
            outb(COM1_PORT + 2, 0xC7);
            // IRQ ready
            outb(COM1_PORT + 4, 0x0B);
        }
        SerialWriter
    }

    pub fn print_char(&mut self, c: u8) {
        unsafe {
            while (inb(COM1_PORT + 5) & 0x20) == 0 {}
            outb(COM1_PORT, c);
        }
    }

    pub fn print_str(&mut self, s: &str) {
        for c in s.bytes() {
            self.print_char(c);
        }
    }
}

unsafe fn outb(port: u16, val: u8) {
    core::arch::asm!("out dx, al", in("dx") port, in("al") val, options(nomem, nostack));
}

unsafe fn inb(port: u16) -> u8 {
    let mut val: u8;
    core::arch::asm!("in al, dx", out("al") val, in("dx") port, options(nomem, nostack));
    val
}
