//! Interrupt Descriptor Table

const IDT_ENTRIES: usize = 256;

#[derive(Copy, Clone)]
#[repr(C, packed)]
struct IDTEntry {
    offset_low: u16,
    selector: u16,
    ist: u8,
    flags: u8,
    offset_mid: u16,
    offset_high: u32,
    reserved: u32,
}

impl IDTEntry {
    const fn new(handler: u64, sel: u16, flags: u8) -> Self {
        IDTEntry {
            offset_low: handler as u16,
            offset_mid: (handler >> 16) as u16,
            offset_high: (handler >> 32) as u32,
            selector: sel,
            ist: 0,
            flags,
            reserved: 0,
        }
    }
}

#[repr(C, packed)]
struct IDTDescriptor {
    size: u16,
    offset: u64,
}

static mut IDT: [IDTEntry; IDT_ENTRIES] = [IDTEntry {
    offset_low: 0,
    selector: 0,
    ist: 0,
    flags: 0,
    offset_mid: 0,
    offset_high: 0,
    reserved: 0,
}; IDT_ENTRIES];

pub unsafe fn init_idt() {
    // Set up exception handlers (0-31)
    set_idt_gate(0, divide_error_handler as u64, 0x08, 0x8E);
    set_idt_gate(6, invalid_opcode_handler as u64, 0x08, 0x8E);
    set_idt_gate(13, general_protection_handler as u64, 0x08, 0x8E);
    set_idt_gate(14, page_fault_handler as u64, 0x08, 0x8E);

    let descriptor = IDTDescriptor {
        size: core::mem::size_of::<[IDTEntry; IDT_ENTRIES]>() as u16 - 1,
        offset: &IDT as *const _ as u64,
    };
    lidt(&descriptor);
}

unsafe fn set_idt_gate(entry: usize, handler: u64, selector: u16, flags: u8) {
    IDT[entry] = IDTEntry::new(handler, selector, flags);
}

unsafe fn lidt(descriptor: &IDTDescriptor) {
    core::arch::asm!("lidt [{0}]", in(reg) descriptor, options(readonly));
}

// Exception handler stubs - these just halt for now
// In a full implementation, these would log the error and attempt recovery

#[no_mangle]
extern "C" fn divide_error_handler() {
    loop { unsafe { core::arch::asm!("hlt"); } }
}

#[no_mangle]
extern "C" fn invalid_opcode_handler() {
    loop { unsafe { core::arch::asm!("hlt"); } }
}

#[no_mangle]
extern "C" fn general_protection_handler() {
    loop { unsafe { core::arch::asm!("hlt"); } }
}

#[no_mangle]
extern "C" fn page_fault_handler() {
    let cr2: u64;
    unsafe { core::arch::asm!("mov {0}, cr2", out(reg) cr2); }
    let _ = cr2; // CR2 contains the faulting address
    loop { unsafe { core::arch::asm!("hlt"); } }
}
