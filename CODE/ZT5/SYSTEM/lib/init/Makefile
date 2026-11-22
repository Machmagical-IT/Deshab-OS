# Zratro OS - 用户程序 Makefile

CC = gcc
AS = gcc
LD = ld

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -fno-builtin -nostdlib -nostdinc \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -mcmodel=small -m64 \
         -Wall -Wextra -O2 -pipe -I include

LDFLAGS = -static -nostdlib -T user.ld

BINDIR = ../../prgm/sys

.PHONY: all clean

all: $(BINDIR)/init

$(BINDIR)/init: crt0.o init.o libc.o
	@mkdir -p $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ $^

init.o: init.c
	$(CC) $(CFLAGS) -c -o $@ $<

shell.o: shell.c
	$(CC) $(CFLAGS) -c -o $@ $<

libc.o: libc.c
	$(CC) $(CFLAGS) -c -o $@ $<

crt0.o: crt0.S
	$(AS) -c -o $@ $<

clean:
	rm -f *.o $(BINDIR)/init