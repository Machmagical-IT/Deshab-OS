# Zratro OS - File Manager Makefile
# Compiles to: SYSTEM/prgm/windowprogram/fm

CC = gcc
LD = ld

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -fno-builtin -nostdlib -nostdinc \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -mcmodel=small -m64 \
         -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
         -O2 -pipe \
         -mgeneral-regs-only -fno-tree-vectorize

LDFLAGS = -static -nostdlib -T fm.ld

BINDIR = ../../prgm/windowprogram
TARGET = $(BINDIR)/fm

.PHONY: all clean

all: $(TARGET)

$(TARGET): fm.o syscall_asm.o
	@mkdir -p $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ $^

fm.o: fm.c
	$(CC) $(CFLAGS) -c -o $@ $<

syscall_asm.o: syscall_asm.S
	$(CC) -c -o $@ $<

clean:
	rm -f *.o $(TARGET)
