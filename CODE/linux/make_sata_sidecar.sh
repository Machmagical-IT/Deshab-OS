#!/usr/bin/env bash
# make_sata_sidecar.sh — 重建 .build_tmp/sata_fat32_dsk.img（AHCI 边车测试盘）
# 用法: sudo bash make_sata_sidecar.sh <SYSTEM_DIR> <OUT_IMG>
# 说明: 纯 FAT32（无 GPT），经 ahci0 provider 读 BPB。
#   只拷贝运行期需要的子树（SYSTEM/ESP 里的 system/，即 DSK 启动所需），
#   排除 boot/ EFI/ limine/ 等仅引导阶段需要的大文件。
set -euo pipefail

SRC="${1:?usage: $0 <SYSTEM_DIR> <OUT_IMG>}"
IMG="${2:?usage: $0 <SYSTEM_DIR> <OUT_IMG>}"
ESP="$SRC/ESP"

command -v mkfs.vfat >/dev/null || { echo "mkfs.vfat missing" >&2; exit 1; }
if [[ ! -d "$ESP/system" ]]; then
    echo "[sidecar] ERROR: $ESP/system missing (SYSTEM/ESP/system is required)" >&2
    exit 1
fi

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

# 仅运行期子树；boot/EFI/limine 是引导分区专属（rootfs 镜像 GB 级）
cp -r "$ESP/system" "$STAGE"/

need_kb=$(du -sk "$STAGE" | cut -f1)
size_mb=$(( need_kb / 1024 * 2 + 64 ))
[ "$size_mb" -lt 64 ] && size_mb=64

rm -f "$IMG"
truncate -s "${size_mb}M" "$IMG"
mkfs.vfat -F 32 -n DESHAB "$IMG" >/dev/null

MNT=$(mktemp -d)
mount -o loop "$IMG" "$MNT"
cp -r "$STAGE"/. "$MNT"/
sync
umount "$MNT"
rmdir "$MNT"
echo "[sidecar] rebuilt: $IMG (${size_mb}MB FAT32)"
