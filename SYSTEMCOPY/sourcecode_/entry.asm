; =============================================================================
;  Nutro+OS - kernel entry point
;  Called by the stage 2 loader in 32-bit protected mode with:
;      EAX = physical address of the boot information block
;      CS  = 0x08, DS/ES/FS/GS = 0x10 (flat 4 GB segments)
; =============================================================================
BITS 32

global _start
extern kernel_main
extern __bss_start
extern __bss_end

section .text.start progbits alloc exec nowrite align=16
_start:
    cli
    mov esp, stack_top
    mov ebx, eax                    ; keep the boot info pointer

    ; ---- initialise the FPU / SSE so compiler-generated code can use it --
    mov eax, cr0
    and eax, ~(1 << 2)              ; clear EM
    or  eax, (1 << 1)               ; set MP
    mov cr0, eax
    mov eax, cr4
    or  eax, (1 << 9) | (1 << 10)   ; OSFXSR | OSXMMEXCPT
    mov cr4, eax
    fninit

    ; ---- zero the BSS ---------------------------------------------------
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    mov edx, ecx
    shr ecx, 2
    cld
    rep stosd
    mov ecx, edx
    and ecx, 3
    rep stosb

    ; ---- load the kernel's own GDT --------------------------------------
    lgdt [gdt_ptr]
    jmp 0x08:.reload_cs
.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- enter the kernel ------------------------------------------------
    mov eax, ebx
    push eax
    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

section .data progbits alloc noexec write align=16
gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00      ; 0x08: 32-bit code, base 0, limit 4 GB
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00      ; 0x10: 32-bit data, base 0, limit 4 GB
gdt_end:
gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt

section .bss nobits alloc noexec write align=16
align 16
stack_bottom:
    resb 65536
stack_top: