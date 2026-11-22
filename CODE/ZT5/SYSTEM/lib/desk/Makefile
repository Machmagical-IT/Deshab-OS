# Zratro OS - Desktop Program Makefile
#
# Builds the complete desktop environment as a single binary:
#   desk.c (main desktop + window manager + file manager)
#
# All components are compiled into one binary for simplicity.
# The separate zrrWindowsManager and zrrExfileManager libraries
# are not linked - their functionality is integrated into desk.c.

CC = gcc
LD = ld

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -fno-builtin -nostdlib -nostdinc \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -mcmodel=small -m64 \
         -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
         -O2 -pipe \
         -mgeneral-regs-only -fno-tree-vectorize

LDFLAGS = -static -nostdlib -T desk.ld

BINDIR = ../../prgm/sys
TARGET = $(BINDIR)/desk

.PHONY: all clean

all: $(TARGET)

$(TARGET): desk.o syscall_asm.o
	@mkdir -p $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ $^

desk.o: desk.c
	$(CC) $(CFLAGS) -c -o $@ $<

syscall_asm.o: syscall_asm.S
	$(CC) -c -o $@ $<

clean:
	rm -f *.o $(TARGET)
