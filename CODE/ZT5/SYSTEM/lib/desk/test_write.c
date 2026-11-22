/* Minimal test: just call sys_write to see if it works */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
#define NULL ((void*)0)

static long sys_write(int fd, const void *buf, long count) {
    long r;
    register long rax __asm__("rax") = 2; /* SYS_WRITE */
    register long rdi __asm__("rdi") = (long)fd;
    register long rsi __asm__("rsi") = (long)buf;
    register long rdx __asm__("rdx") = (long)count;
    __asm__ __volatile__("int $0x80" 
        : "=a"(r) 
        : "a"(rax), "D"(rdi), "S"(rsi), "d"(rdx) 
        : "rcx", "r11", "memory");
    return r;
}

void __attribute__((optimize("O0"))) _start(void) {
    const char msg[] = "[TEST] sys_write is working!\n";
    long ret = sys_write(2, msg, sizeof(msg)-1);
    
    /* Infinite loop */
    while(1) {
        for(volatile int j=0; j<100000000; j++);
    }
}
