#!/bin/bash
# build_esp.sh — 生成演示用最小 FAT32 卷 esp.img（仅供桌面宿主读取）
#
# 内容 = 真实启动/桌面会读取的资源 + 与发行镜像相同的菜单/配置文本。
# 不修改 SYSTEM/ 源文件；FUCK 只在临时副本末尾追加演示覆盖段。
set -e

ROOT=/mnt/d/Code/Deshab
SRC=$ROOT/SYSTEM/ESP
STAGE=$ROOT/.build_tmp/demo_esp
IMG=$ROOT/INSTALL/demo/esp.img
SIZE_MB=48

rm -rf "$STAGE"
mkdir -p "$STAGE/system/deshab64/desktop/textures" "$STAGE/system/font"

cp "$SRC/STARTMENU.CNF" "$SRC/PEAPPS.CNF" "$SRC/LINUXAPP.CNF" "$STAGE/" 2>/dev/null || true
cp "$SRC/system/font/simhei_16.dbf" "$STAGE/system/font/"
cp "$SRC/system/deshab64/desktop/textures/back.rgba" \
   "$SRC/system/deshab64/desktop/textures/startMenuLogo.rgba" \
   "$SRC/system/deshab64/desktop/textures/bootlogo.rgba" \
   "$STAGE/system/deshab64/desktop/textures/"
cp "$SRC/system/deshab64/FUCK" "$STAGE/system/deshab64/FUCK"

# 演示覆盖：关闭整蛊自动触发与动效自检（桌面外壳本身与原版一致）
printf '\n[desktop]\nmotion_selftest=0\n\n[prank]\nenable=0\n' >> "$STAGE/system/deshab64/FUCK"

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=$SIZE_MB status=none
mkfs.vfat -F 32 -s 1 -S 512 -n DESHABDEMO "$IMG" >/dev/null
export MTOOLS_SKIP_CHECK=1
mcopy -s -i "$IMG" "$STAGE"/* ::/
mdir -i "$IMG" ::/ | head -20
ls -l "$IMG"
