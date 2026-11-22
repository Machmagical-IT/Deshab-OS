//! VGA Text Mode Driver (0xB8000)

const VGA_WIDTH: usize = 80;
const VGA_HEIGHT: usize = 25;
const VGA_MEMORY: *mut u8 = 0xB8000 as *mut u8;

const BLACK: u8 = 0x0;
const LIGHT_GREEN: u8 = 0xA;
const YELLOW: u8 = 0xE;
const WHITE: u8 = 0xF;

pub struct VGAWriter {
    row: usize,
    col: usize,
}

impl VGAWriter {
    pub const fn new() -> Self {
        VGAWriter { row: 0, col: 0 }
    }

    pub fn print_char(&mut self, c: u8) {
        match c {
            b'\n' => self.new_line(),
            b'\r' => self.col = 0,
            b'\t' => {
                let tab = 4 - (self.col % 4);
                for _ in 0..tab {
                    self.write_byte(b' ', LIGHT_GREEN);
                }
            }
            _ => self.write_byte(c, LIGHT_GREEN),
        }
    }

    pub fn print_str(&mut self, s: &str) {
        for c in s.bytes() {
            self.print_char(c);
        }
    }

    fn write_byte(&mut self, c: u8, color: u8) {
        if self.col >= VGA_WIDTH {
            self.new_line();
        }
        if self.row >= VGA_HEIGHT {
            self.scroll();
        }

        let offset = (self.row * VGA_WIDTH + self.col) * 2;
        unsafe {
            VGA_MEMORY.add(offset).write_volatile(c);
            VGA_MEMORY.add(offset + 1).write_volatile(color);
        }
        self.col += 1;
    }

    fn new_line(&mut self) {
        self.col = 0;
        self.row += 1;
    }

    fn scroll(&mut self) {
        for row in 1..VGA_HEIGHT {
            for col in 0..VGA_WIDTH {
                let src = (row * VGA_WIDTH + col) * 2;
                let dst = ((row - 1) * VGA_WIDTH + col) * 2;
                unsafe {
                    let ch = VGA_MEMORY.add(src).read_volatile();
                    let attr = VGA_MEMORY.add(src + 1).read_volatile();
                    VGA_MEMORY.add(dst).write_volatile(ch);
                    VGA_MEMORY.add(dst + 1).write_volatile(attr);
                }
            }
        }
        // Clear last line
        for col in 0..VGA_WIDTH {
            let offset = ((VGA_HEIGHT - 1) * VGA_WIDTH + col) * 2;
            unsafe {
                VGA_MEMORY.add(offset).write_volatile(b' ');
                VGA_MEMORY.add(offset + 1).write_volatile(BLACK);
            }
        }
        self.row = VGA_HEIGHT - 1;
    }
}
