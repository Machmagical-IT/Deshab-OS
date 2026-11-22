#!/bin/bash
# ============================================================================
# patch_rootfs_pacman.sh — 给已构建的 Arch rootfs 镜像打上 pacman 可写层修复
#
# 为什么不直接跑 build_arch_rootfs.sh：
#   那条路要重新解包 121MB bootstrap + 走 pacman 装 python/wifi 工具，耗时
#   且依赖网络。已有镜像里 139 个包的实体都在，缺的只是三样东西：
#     1. /usr/local/share/deshab/pacman-local  （只读母本库）
#     2. 启动 wrapper 里的 /usr /etc /var overlay 挂载
#   本脚本只做这两件事，原地改镜像，几分钟完成。
#
# 用法：sudo bash patch_rootfs_pacman.sh [rootfs.img] [bootstrap.tar.zst]
# ============================================================================
set -Eeuo pipefail

IMG="${1:-/mnt/d/Code/Deshab/SYSTEM/ESP/boot/linux-rootfs.img}"
BOOTSTRAP="${2:-/root/arch_rootfs_work/archlinux-bootstrap.tar.zst}"
WRAPPER_SRC="/mnt/d/Code/Deshab/CODE/linux/initramfs/guest_boot.sh"

MNT=/mnt/rootfspatch
WORK=/root/rootfspatch
LOOP=""

cleanup() {
    set +e
    umount "$MNT/dev"  2>/dev/null
    umount "$MNT/proc" 2>/dev/null
    umount "$MNT/sys"  2>/dev/null
    umount "$MNT"      2>/dev/null
    [[ -n "$LOOP" ]] && losetup -d "$LOOP" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "[patch] image: $IMG"
[[ -f "$IMG" ]] || { echo "[patch] ERROR: image not found" >&2; exit 1; }
[[ -f "$WRAPPER_SRC" ]] || { echo "[patch] ERROR: wrapper source not found: $WRAPPER_SRC" >&2; exit 1; }

mkdir -p "$MNT" "$WORK"

# 1. 从 bootstrap 取出干净的 pacman local 库（139 包 / ~4.3MB）
if [[ ! -d "$WORK/local" ]]; then
    echo "[patch] extracting pristine pacman local db from bootstrap..."
    mkdir -p "$WORK/db"
    tar --zstd -xf "$BOOTSTRAP" -C "$WORK/db" root.x86_64/var/lib/pacman/local 2>/dev/null \
        || { echo "[patch] ERROR: cannot extract local db from bootstrap" >&2; exit 1; }
    mv "$WORK/db/root.x86_64/var/lib/pacman/local" "$WORK/local"
    rm -rf "$WORK/db"
fi
echo "[patch] pristine db: $(find "$WORK/local" -maxdepth 1 -mindepth 1 -type d | wc -l) pkgs"

# 2. 原地挂载镜像（可写，用完卸载）
LOOP=$(losetup -f --show "$IMG") || { echo "[patch] ERROR: losetup failed" >&2; exit 1; }
echo "[patch] loop=$LOOP"
mount -t ext4 "$LOOP" "$MNT" || { echo "[patch] ERROR: mount failed" >&2; exit 1; }

# 3. 放母本库
PRISTINE="$MNT/usr/local/share/deshab/pacman-local"
rm -rf "$PRISTINE"
mkdir -p "$(dirname "$PRISTINE")"
cp -a "$WORK/local" "$PRISTINE"
echo "[patch] installed pristine db -> /usr/local/share/deshab/pacman-local"

# 4. 换启动 wrapper：整份替换，内容来自仓库里的 guest_boot.sh
#    原来的 utsm_exec_daemon 若已是 wrapper（含 .real），说明打过补丁，保留 .real。
DAEMON="$MNT/usr/local/bin/utsm_exec_daemon"
if [[ -f "$MNT/usr/local/bin/utsm_exec_daemon.real" ]]; then
    echo "[patch] daemon.real already present (previous wrapper?)"
elif [[ -f "$DAEMON" ]]; then
    cp -a "$DAEMON" "$MNT/usr/local/bin/utsm_exec_daemon.real"
    echo "[patch] backed up current daemon -> utsm_exec_daemon.real"
else
    echo "[patch] ERROR: neither daemon nor daemon.real present" >&2
    exit 1
fi

# wrapper 由 guest_boot.sh 渲染（其中 %DAEMON% 换成真 daemon 路径）
sed 's|%DAEMON%|/usr/local/bin/utsm_exec_daemon.real|g' "$WRAPPER_SRC" > "$DAEMON"
chmod 0755 "$DAEMON"
echo "[patch] installed boot wrapper -> /usr/local/bin/utsm_exec_daemon"

# 5. 保持 /var/lib/pacman → /tmp/pacman/lib 的符号链接（若还在就别动）
for p in var/lib/pacman var/cache/pacman; do
    if [[ -L "$MNT/$p" ]]; then
        echo "[patch] $p already a symlink -> $(readlink "$MNT/$p")"
    elif [[ -d "$MNT/$p" ]]; then
        echo "[patch] WARNING: $p is a real dir, leaving as-is"
    fi
done

sync
echo "[patch] done"
