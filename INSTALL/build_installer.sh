#!/usr/bin/env bash
#
# INSTALL/build_installer.sh —— 构建 DeshabOS 安装器 Hybrid ISO（BIOS + UEFI）
#
# 产物：
#   INSTALL/installer/bzImage            最简安装内核（内置有线网卡/NVMe/AHCI/USB）
#   INSTALL/installer/initrd.gz          busybox initramfs（含 init / udhcpc.script）
#   INSTALL/deshab-installer.iso         grub-mkrescue 生成的混合镜像
#
# 用法（WSL 内）：
#   bash INSTALL/build_installer.sh
#   LINUX_SRC=/path/to/linux-6.6 bash INSTALL/build_installer.sh   # 指定内核源码树
#
# 注意：O= 外部构建要求源码树干净（不能有 in-tree .config）。本仓库使用的
# ~/linux-6.6 是 guest 内核的 in-tree 工作树，已有 .config，故这里直接对
# LINUX_SRC 指定的树做 in-tree 构建——请指向一份专用副本，例如：
#   cp -a ~/linux-6.6 ~/linux-6.6-installer && make -C ~/linux-6.6-installer mrproper
#
# 依赖：build-essential bc flex bison libelf-dev libssl-dev cpio gzip \
#       grub-pc-bin grub-efi-amd64-bin xorriso mtools
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LINUX_SRC="${LINUX_SRC:-$HOME/linux-6.6-installer}"
OUT_DIR="$SCRIPT_DIR/installer"
ISOTREE="$SCRIPT_DIR/.isotree"
INITRAMFS_SRC="$SCRIPT_DIR/installer/initramfs"
ISO_OUT="$SCRIPT_DIR/deshab-installer.iso"
BUSYBOX_URL="https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox"
BUSYBOX_CACHE="$REPO_ROOT/CODE/linux/.cache/busybox"

log() { echo "[installer-build] $*"; }
die() { echo "[installer-build] ERROR: $*" >&2; exit 1; }

# ---------- 0. 依赖检查 ----------
[ -f "$LINUX_SRC/Makefile" ] || die "找不到内核源码树: $LINUX_SRC（设置 LINUX_SRC）"
for t in gcc make cpio gzip grub-mkrescue xorriso go; do
    command -v "$t" >/dev/null 2>&1 || die "缺少工具: $t（apt install build-essential bc flex bison libelf-dev libssl-dev cpio gzip grub-pc-bin grub-efi-amd64-bin xorriso mtools golang-go）"
done
[ -d /usr/lib/grub/i386-pc ]    || die "缺少 grub-pc-bin（BIOS 引导）"
[ -d /usr/lib/grub/x86_64-efi ] || die "缺少 grub-efi-amd64-bin（UEFI 引导）"

mkdir -p "$OUT_DIR"

# ---------- 1. 内核 ----------
log "配置内核（installer_defconfig）..."
cp "$SCRIPT_DIR/configs/installer_defconfig" "$LINUX_SRC/arch/x86/configs/installer_defconfig"
make -C "$LINUX_SRC" installer_defconfig >/dev/null

log "编译 bzImage（-j$(nproc)）..."
make -C "$LINUX_SRC" -j"$(nproc)" bzImage
cp -f "$LINUX_SRC/arch/x86/boot/bzImage" "$OUT_DIR/bzImage"
log "bzImage: $(du -h "$OUT_DIR/bzImage" | cut -f1)"

# ---------- 2. initramfs ----------
BUSYBOX="$BUSYBOX_CACHE"
if [ ! -x "$BUSYBOX" ]; then
    log "下载静态 busybox ..."
    mkdir -p "$(dirname "$BUSYBOX")"
    curl -fL --retry 3 -o "$BUSYBOX" "$BUSYBOX_URL"
    chmod +x "$BUSYBOX"
fi

STAGE="$SCRIPT_DIR/.build/initramfs"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/etc/ssl/certs" "$STAGE/dev" "$STAGE/proc" "$STAGE/sys" "$STAGE/tmp"
cp "$BUSYBOX" "$STAGE/bin/busybox"
chmod +x "$STAGE/bin/busybox"
cp "$INITRAMFS_SRC/init" "$STAGE/init"
cp "$INITRAMFS_SRC/udhcpc.script" "$STAGE/etc/udhcpc.script"
chmod +x "$STAGE/init" "$STAGE/etc/udhcpc.script"

log "编译静态下载器 dl（busybox TLS 不可用）..."
( cd "$SCRIPT_DIR/installer/dl" && CGO_ENABLED=0 GOOS=linux GOARCH=amd64 \
    go build -trimpath -ldflags '-s -w' -o "$STAGE/bin/dl" . ) || die "dl 构建失败"
chmod +x "$STAGE/bin/dl"

CA_BUNDLE="${CA_BUNDLE:-/etc/ssl/certs/ca-certificates.crt}"
[ -f "$CA_BUNDLE" ] || die "缺少 CA 证书包: $CA_BUNDLE"
cp "$CA_BUNDLE" "$STAGE/etc/ssl/certs/ca-certificates.crt"

log "生成 initramfs ..."
GEN_INIT_CPIO="$LINUX_SRC/usr/gen_init_cpio"
if [ ! -x "$GEN_INIT_CPIO" ]; then
    make -C "$LINUX_SRC" usr/ >/dev/null
fi
[ -x "$GEN_INIT_CPIO" ] || die "gen_init_cpio 构建失败"

SPEC="$SCRIPT_DIR/.build/initramfs.spec"
{
    echo "dir /bin 755 0 0"
    echo "dir /etc 755 0 0"
    echo "dir /etc/ssl 755 0 0"
    echo "dir /etc/ssl/certs 755 0 0"
    echo "dir /dev 755 0 0"
    echo "dir /proc 755 0 0"
    echo "dir /sys 755 0 0"
    echo "dir /tmp 1777 0 0"
    echo "nod /dev/console 600 0 0 c 5 1"
    echo "nod /dev/null 666 0 0 c 1 3"
    echo "nod /dev/ttyS0 600 0 0 c 4 64"
    echo "file /init $STAGE/init 755 0 0"
    echo "file /etc/udhcpc.script $STAGE/etc/udhcpc.script 755 0 0"
    echo "file /etc/ssl/certs/ca-certificates.crt $STAGE/etc/ssl/certs/ca-certificates.crt 644 0 0"
    echo "file /bin/busybox $STAGE/bin/busybox 755 0 0"
    echo "file /bin/dl $STAGE/bin/dl 755 0 0"
    for app in sh mount umount mkdir rm cat ls ip udhcpc wget sed grep awk sort head tail \
               wc dd sha256sum gzip mdev poweroff reboot sleep clear tr cut basename printf \
               dmesg sync stty; do
        echo "slink /bin/$app /bin/busybox 777 0 0"
    done
} > "$SPEC"

"$GEN_INIT_CPIO" "$SPEC" | gzip -9 > "$OUT_DIR/initrd.gz"
log "initrd.gz: $(du -h "$OUT_DIR/initrd.gz" | cut -f1)"

# ---------- 3. ISO ----------
log "组装 ISO 目录树 ..."
rm -rf "$ISOTREE"
mkdir -p "$ISOTREE/boot/grub"
cp "$OUT_DIR/bzImage"   "$ISOTREE/boot/bzImage"
cp "$OUT_DIR/initrd.gz" "$ISOTREE/boot/initrd.gz"

cat > "$ISOTREE/boot/grub/grub.cfg" <<'EOF'
# 串口终端：无显示器装机 / 自动化测试都能操作 grub
serial --unit=0 --speed=115200
terminal_input  console serial
terminal_output console serial

set timeout=10
set default=0

# console= 的顺序决定"首选控制台"（最后一个 = 首选）：/dev/console 会
# 指向它。真机没有串口时，若 ttyS0 是首选，/init 的 echo/read 全部写到
# 不存在的串口 —— 屏幕上只剩内核日志，随后静默关机（表现为"卡在内核"）。
# 因此默认项把 tty0 放最后；串口项反过来并显式告诉 /init 用串口做交互。
menuentry "DeshabOS Installer (VGA console)" {
    linux /boot/bzImage console=ttyS0,115200 console=tty0
    initrd /boot/initrd.gz
}

menuentry "DeshabOS Installer (serial console, 115200)" {
    linux /boot/bzImage console=tty0 console=ttyS0,115200 installer.console=serial
    initrd /boot/initrd.gz
}

menuentry "DeshabOS Installer (指定镜像 URL，测试用)" {
    linux /boot/bzImage console=ttyS0,115200 console=tty0 installer.url=
    initrd /boot/initrd.gz
}
EOF

log "grub-mkrescue ..."
rm -f "$ISO_OUT"
grub-mkrescue -o "$ISO_OUT" "$ISOTREE" -- -volid DESHAB_INSTALL >/dev/null 2>&1 \
    || die "grub-mkrescue 失败"

log "完成：$ISO_OUT ($(du -h "$ISO_OUT" | cut -f1))"
log "写 U 盘：Rufus（DD 模式）或  dd if=$ISO_OUT of=/dev/sdX bs=4M conv=fsync"
