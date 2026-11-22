/* syscall_asm.S - Assembly syscall wrappers for desk */
/* Uses GNU assembler syntax */

.text
.globl sys_write_asm

/* long sys_write_asm(int fd, const void *buf, long count) */
/* On entry (System V ABI): rdi=fd, rsi=buf, rdx=count */
sys_write_asm:
    mov $2, %rax          /* SYS_WRITE = 2 */
    /* rdi, rsi, rdx already have correct values from caller */
    int $0x80
    ret
