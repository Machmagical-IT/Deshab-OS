# Zratro OS - Window Manager (zrdemage) Makefile
# Compiles to: SYSTEM/system/zrdemage

CC = gcc
LD = ld

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -fno-builtin -nostdlib -nostdinc \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -mcmodel=small -m64 \
         -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
         -O2 -pipe \
         -mgeneral-regs-only -fno-tree-vectorize

LDFLAGS = -static -nostdlib -T wm.ld

BINDIR = ../../prgm/sys
TARGET = $(BINDIR)/zrdemage

.PHONY: all clean

all: $(TARGET)

$(TARGET): wm.o syscall_asm.o
	$(LD) $(LDFLAGS) -o $@ $^

wm.o: wm.c
	$(CC) $(CFLAGS) -c -o $@ $<

syscall_asm.o: syscall_asm.S
	$(CC) -c -o $@ $<

clean:
	rm -f *.o $(TARGET)
