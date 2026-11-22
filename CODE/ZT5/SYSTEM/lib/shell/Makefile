# Makefile for Zratro Shell
# Compiles to: SYSTEM/prgm/shell (user program)

CC = gcc
LD = ld

CFLAGS = -nostdlib -nostartfiles -static -Os -Wall -Wextra
CFLAGS += -fno-builtin -fno-stack-protector
CFLAGS += -m64 -mcmodel=large
CFLAGS += -I.

LDFLAGS = -static -nostdlib -T ../init/user.ld -e _start

# Source files
C_SRCS = main.c syscall.c termios.c io.c history.c string.c commands.c parser.c
ASM_SRCS = crt0.s
C_OBJS = $(C_SRCS:.c=.o)
ASM_OBJS = $(ASM_SRCS:.s=.o)
OBJS = $(ASM_OBJS) $(C_OBJS)

# Output
BINDIR = ../../prgm
TARGET = $(BINDIR)/shell

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c zsh.h
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.s
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(C_OBJS) $(ASM_OBJS) $(TARGET)
