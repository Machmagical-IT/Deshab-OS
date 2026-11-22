CC = gcc
LD = ld

# 检测操作系统
ifeq ($(OS),Windows_NT)
    # Windows/MinGW 环境使用 medium 代码模型（避免 PIC 冲突）
    CMODEL = -mcmodel=medium
    # 禁用 MinGW 特定的类型定义
    EXTRA_CFLAGS = -Wno-builtin-declaration-mismatch
else
    # Linux 使用 kernel 代码模型
    CMODEL = -mcmodel=kernel
    EXTRA_CFLAGS =
endif

# 基础编译选项
BASE_CFLAGS = -ffreestanding -fno-pic -fno-pie \
              -mno-red-zone \
              -mno-mmx -mno-sse -mno-sse2 $(CMODEL) \
              -Wall -Wextra -O2 -pipe -I include $(EXTRA_CFLAGS)

# 安全加固选项
SECURITY_CFLAGS = -fstack-protector-strong \
                  -D_FORTIFY_SOURCE=2 \
                  -fno-delete-null-pointer-checks \
                  -fno-strict-overflow

# 完整编译选项 (安全选项可选启用)
CFLAGS = $(BASE_CFLAGS) $(SECURITY_CFLAGS) -Wno-error

LDFLAGS = -static -nostdlib -T linker.ld

SRCDIR = .
OBJDIR = obj
ISODIR = iso

LIMINE_DIR = /usr
LIMINE_SHARE = /usr/share/limine

# C 源文件 (排除有问题的文件)
SOURCES_C = $(filter-out deadlock_free.c epoll.c futex.c, $(wildcard *.c))
OBJECTS_C = $(SOURCES_C:.c=.o)

# VMM 子目录
VMM_DIR = vmm
SOURCES_VMM = $(wildcard $(VMM_DIR)/*.c)
OBJECTS_VMM = $(SOURCES_VMM:.c=.o)

# 中文支持模块 (如果在源码目录中)
ifeq ($(wildcard chinese_font.c),chinese_font.c)
SOURCES_ZH = chinese_font.c
OBJECTS_ZH = $(SOURCES_ZH:.c=.o)
endif

# Rust 内核模块
RUST_KERNEL_DIR = ../rust_kernel
RUST_KERNEL_LIB = $(RUST_KERNEL_DIR)/target/x86_64-unknown-none/release/libzratros_kernel.a

# 添加 Rust 库支持
ifeq ($(RUST_ENABLED),1)
CFLAGS += -DRUST_ENABLED
LDFLAGS += -L$(RUST_KERNEL_DIR)/target/x86_64-unknown-none/release -lzratros_kernel
endif

# 汇编源文件
SOURCES_ASM = $(wildcard *_asm.S) $(wildcard smp_asm.S) $(wildcard syscall_fast.S)
OBJECTS_ASM = $(SOURCES_ASM:.S=.o)

# 所有目标文件
OBJECTS = $(OBJECTS_C) $(OBJECTS_ASM) $(OBJECTS_VMM) $(OBJECTS_ZH)

KERNEL = ../../system/knl.bin

.PHONY: all clean iso rust-kernel

all: $(KERNEL)

# Rust 内核模块目标
rust-kernel:
	@echo "Building Rust kernel module..."
	$(MAKE) -C $(RUST_KERNEL_DIR) build

# 如果需要 Rust，先编译 Rust 模块
ifeq ($(RUST_ENABLED),1)
$(KERNEL): rust-kernel
endif

$(KERNEL): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) -c -o $@ $<

$(VMM_DIR)/%.o: $(VMM_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

ISO = ../../zratro-v1.1.1.iso

iso: $(KERNEL)
	rm -rf $(ISODIR)
	mkdir -p $(ISODIR)/boot/EFI/BOOT
	mkdir -p $(ISODIR)/system/res/up
	mkdir -p $(ISODIR)/system/firmware
	mkdir -p $(ISODIR)/system/internet
	mkdir -p $(ISODIR)/prgm/sys
	cp $(KERNEL) $(ISODIR)/system/knl.bin
	cp ../../boot/limine.conf $(ISODIR)/boot/
	cp $(LIMINE_SHARE)/limine-bios.sys $(ISODIR)/boot/
	cp $(LIMINE_SHARE)/limine-bios-cd.bin $(ISODIR)/boot/
	cp $(LIMINE_SHARE)/limine-uefi-cd.bin $(ISODIR)/boot/
	cp $(LIMINE_SHARE)/BOOTX64.EFI $(ISODIR)/boot/EFI/BOOT/
	if [ -f ../../system/res/up/0x1a.bmp ]; then \
		cp ../../system/res/up/0x1a.bmp $(ISODIR)/system/res/up/; \
	fi
	if [ -f ../../system/zratro.conf ]; then \
		cp ../../system/zratro.conf $(ISODIR)/system/; \
	fi
	if [ -f ../../prgm/sys/init ]; then \
		cp ../../prgm/sys/init $(ISODIR)/prgm/sys/; \
	fi
	if [ -f ../../prgm/sys/shell ]; then \
		cp ../../prgm/sys/shell $(ISODIR)/prgm/sys/; \
	fi
	if [ -f ../../prgm/sys/desk ]; then \
		cp ../../prgm/sys/desk $(ISODIR)/prgm/sys/; \
	fi
	if [ -f ../../system/aknl.bin ]; then \
		cp ../../system/aknl.bin $(ISODIR)/system/; \
	fi
	# WiFi firmware
	if [ -f ../../system/firmware/rtl8188eufw.bin ]; then \
		cp ../../system/firmware/rtl8188eufw.bin $(ISODIR)/system/firmware/; \
	fi
	if [ -f ../../system/internet/e1w.cfg ]; then \
		cp ../../system/internet/e1w.cfg $(ISODIR)/system/internet/; \
	fi
	xorriso -as mkisofs -b boot/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISODIR) -o $(ISO)
	limine bios-install $(ISO)

clean:
	rm -f *.o $(KERNEL) $(ISO)
	rm -rf $(ISODIR)
