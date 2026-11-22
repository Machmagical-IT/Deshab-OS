# ZTS7 编译系统
#
# 编译 .zts 模块并自动安装到内核镜像目录
# 自检模块使用 0x00000000 优先级，确保第一个加载

# 编译器
CC = gcc
CFLAGS = -fPIC -O2 -Wall -Wextra
LDFLAGS = -shared

# 源文件
SRCS = $(wildcard *.zts.c)
TARGETS = $(SRCS:.zts.c=.zts)

# 安装路径
INSTALL_PATH = ../../zrr64/zst/p.zst6

.PHONY: all install clean

all: $(TARGETS)
	@echo ""
	@echo "✓ 编译完成: $(TARGETS)"

%.zts: %.zts.c
	@echo "  CC  $<"
	@$(CC) $(CFLAGS) -c -o $*.o $<
	@echo "  LD  $@"
	@$(CC) $(LDFLAGS) -o $@ $*.o
	@strip $@
	@rm -f $*.o

install: all
	@echo "安装到 $(INSTALL_PATH)..."
	@mkdir -p $(INSTALL_PATH)
	@for target in $(TARGETS); do \
		if echo "$$target" | grep -q "^0x"; then \
			cp "$$target" "$(INSTALL_PATH)/$$target"; \
		else \
			cp "$$target" "$(INSTALL_PATH)/$$target"; \
		fi; \
	done
	@echo "✓ 已安装: $(TARGETS)"
	@ls -lh $(INSTALL_PATH)/*.zts

clean:
	@rm -f *.zts *.o
