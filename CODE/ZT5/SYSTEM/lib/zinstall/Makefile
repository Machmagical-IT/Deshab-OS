# Zratro OS - Software Installer (zinstall) Makefile
# CLI:  SYSTEM/prgm/sys/zinstall
# GUI:  SYSTEM/prgm/windowprogram/zinstall

CC = gcc
LD = ld

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -fno-builtin -nostdlib -nostdinc \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -mcmodel=small -m64 \
         -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
         -O2 -pipe \
         -mgeneral-regs-only -fno-tree-vectorize

LDFLAGS_CLI = -static -nostdlib -T zinstall.ld
LDFLAGS_GUI = -static -nostdlib -T zinstall_gui.ld

CLI_BINDIR = ../../prgm/sys
GUI_BINDIR = ../../prgm/windowprogram
CLI_TARGET = $(CLI_BINDIR)/zinstall
GUI_TARGET = $(GUI_BINDIR)/zinstall

.PHONY: all clean

all: $(CLI_TARGET) $(GUI_TARGET)

$(CLI_TARGET): zinstall.o syscall_asm.o
	@mkdir -p $(CLI_BINDIR)
	$(LD) $(LDFLAGS_CLI) -o $@ $^

$(GUI_TARGET): zinstall_gui.o syscall_asm.o
	@mkdir -p $(GUI_BINDIR)
	$(LD) $(LDFLAGS_GUI) -o $@ $^

zinstall.o: zinstall.c
	$(CC) $(CFLAGS) -c -o $@ $<

zinstall_gui.o: zinstall_gui.c
	$(CC) $(CFLAGS) -c -o $@ $<

syscall_asm.o: syscall_asm.S
	$(CC) -c -o $@ $<

clean:
	rm -f *.o $(CLI_TARGET) $(GUI_TARGET)
