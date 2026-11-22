/* syscall_asm.S - Assembly syscall wrappers for fm */
.text
.globl sys_write_asm

sys_write_asm:
    mov $2, %rax
    int $0x80
    ret
