#!/usr/bin/env bash
# Deshab — GUI launcher for Windows (WSL2 + KVM).
#
# WHY WSL2: the Deshab Linux guest runs inside UTSM's VMX root and needs real
# nested VMX. Windows-side QEMU (WHPX/TCG) does not provide it, so the guest
# can never start there ("GUEST NOT RUNNING"). WSL2 exposes /dev/kvm with
# nested VMX; its window is shown on the Windows desktop via WSLg.
#
# Usage: double-click "Deshab (WSL2+KVM).bat" (or the desktop shortcut).
set -euo pipefail

REPO=/mnt/d/Code/DEAICUP/Deshab
SRC_IMG="$REPO/ISO/deshab-dev.img"
NAT_IMG="$HOME/deshab-dev.img"
STAMP="$HOME/.deshab_img_stamp"
LOCK="$HOME/.deshab_gui.lock"
SERIAL_LOG="$REPO/.build_tmp/kvm_gui.log"

# --- Single instance guard: two QEMUs writing one disk image corrupt it. ---
exec 9>"$LOCK"
if ! flock -n 9; then
    echo "[deshab] already running — close the existing QEMU window first."
    exit 0
fi

if [[ ! -e /dev/kvm ]]; then
    echo "[deshab] ERROR: /dev/kvm missing — WSL2 nested virtualization unavailable."
    exit 1
fi

# --- Keep a WSL-native copy of the image (DrvFs /mnt/d locking is unreliable) ---
src_id="$(stat -c '%s-%Y' "$SRC_IMG")"
old_id=""
[[ -f "$STAMP" ]] && old_id="$(cat "$STAMP")"
if [[ "$src_id" != "$old_id" || ! -f "$NAT_IMG" ]]; then
    echo "[deshab] syncing image to WSL disk (one-time per rebuild, ~1 min)..."
    cp -f "$SRC_IMG" "$NAT_IMG"
    cp -f "$REPO/.build_tmp/sata_fat32_dsk.img" "$HOME/sata_fat32_dsk.img" 2>/dev/null || true
    cp -f "$REPO/.build_tmp/nvme_test.img"      "$HOME/nvme_test.img" 2>/dev/null || true
    cp -f "$REPO/.build_tmp/usb_test.img"       "$HOME/usb_test.img" 2>/dev/null || true
    echo "$src_id" > "$STAMP"
fi

mkdir -p "$REPO/.build_tmp"
echo "[deshab] booting (guest + VSCode install take 1-3 min; window opens shortly)..."

cd "$REPO/ISO"
export QEMU_MEM="${QEMU_MEM:-10G}"          # .wslconfig memory=11GB
export IMG_OVERRIDE="$NAT_IMG"
[[ -f "$HOME/sata_fat32_dsk.img" ]] && export SATA_IMG_OVERRIDE="$HOME/sata_fat32_dsk.img"
[[ -f "$HOME/nvme_test.img" ]]      && export NVME_IMG_OVERRIDE="$HOME/nvme_test.img"
[[ -f "$HOME/usb_test.img" ]]       && export USB_IMG_OVERRIDE="$HOME/usb_test.img"
# No QEMU_DISPLAY -> QEMU opens a GTK window on WSLg (Windows desktop).
export QEMU_SERIAL_LOG="$SERIAL_LOG"

exec ./run_qemu_kvm.sh
