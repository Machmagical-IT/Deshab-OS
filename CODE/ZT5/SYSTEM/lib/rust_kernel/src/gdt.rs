//! Global Descriptor Table

#[repr(C, packed)]
struct GDTEntry {
    limit_low: u16,
    base_low: u16,
    base_middle: u8,
    access: u8,
    flags_limit_high: u8,
    base_high: u8,
}

impl GDTEntry {
    const fn new(base: u32, limit: u32, access: u8, flags: u8) -> Self {
        GDTEntry {
            limit_low: (limit & 0xFFFF) as u16,
            base_low: (base & 0xFFFF) as u16,
            base_middle: ((base >> 16) & 0xFF) as u8,
            access,
            flags_limit_high: ((limit >> 16) & 0x0F) as u8 | (flags << 4),
            base_high: ((base >> 24) & 0xFF) as u8,
        }
    }
}

#[repr(C, packed)]
struct GDTDescriptor {
    size: u16,
    offset: u64,
}

static GDT: [GDTEntry; 5] = [
    GDTEntry::new(0, 0, 0, 0),                           // Null descriptor
    GDTEntry::new(0, 0xFFFFF, 0x9A, 0xAF),               // Kernel code (Ring 0)
    GDTEntry::new(0, 0xFFFFF, 0x92, 0xAF),               // Kernel data (Ring 0)
    GDTEntry::new(0, 0xFFFFF, 0xFA, 0xAF),               // User code (Ring 3)
    GDTEntry::new(0, 0xFFFFF, 0xF2, 0xAF),               // User data (Ring 3)
];

pub unsafe fn init_gdt() {
    let descriptor = GDTDescriptor {
        size: core::mem::size_of::<[GDTEntry; 5]>() as u16 - 1,
        offset: &GDT as *const _ as u64,
    };
    lgdt(&descriptor);
}

unsafe fn lgdt(descriptor: &GDTDescriptor) {
    core::arch::asm!("lgdt [{0}]", in(reg) descriptor, options(readonly, nostack));
    // Reload CS segment - use Intel syntax with .intel_syntax noprefix
    core::arch::asm!(
        ".intel_syntax noprefix",
        "push 0x08",              // Kernel code segment selector
        "lea rax, 1f[rip]",
        "push rax",
        "retfq",
        "1:",
        ".att_syntax",
        out("rax") _,
        options(att_syntax),
    );
}
