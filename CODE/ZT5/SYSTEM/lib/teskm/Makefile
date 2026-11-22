# ZratroOS Task Manager Makefile

CC      = gcc
CFLAGS  = -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector \
          -m64 -mno-red-zone -mcmodel=kernel -Wall -Wextra -Wno-implicit-function-declaration \
          -O2 -I../Ztknl/include -I.

LDFLAGS = -T link.ld -nostdlib

SRCS    = teskm.c
OBJS    = $(SRCS:.c=.o)
TARGET  = teskm

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
