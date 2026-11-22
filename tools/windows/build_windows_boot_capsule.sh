#!/usr/bin/env bash
set -Eeuo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <windows-esp-dir> <windows-payload-dir>" >&2
    exit 2
fi

capsule_dir="$1"
payload_dir="$2"
esp_image="$payload_dir/windows-esp.img"

[[ -d "$capsule_dir/EFI" ]] || {
    echo "WIN-E01 boot capsule has no EFI directory: $capsule_dir" >&2
    exit 1
}
command -v mkfs.vfat >/dev/null || {
    echo "WIN-E02 mkfs.vfat is required" >&2
    exit 1
}
command -v mcopy >/dev/null || {
    echo "WIN-E03 mcopy is required" >&2
    exit 1
}

mkdir -p "$payload_dir"
truncate -s 260M "$esp_image"
mkfs.vfat -F 32 -n DWINBOOT "$esp_image" >/dev/null
mcopy -i "$esp_image" -s "$capsule_dir/EFI" ::/

ovmf_code=""
ovmf_vars=""
for candidate in \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/edk2/ovmf/OVMF_CODE_4M.fd; do
    if [[ -f "$candidate" ]]; then ovmf_code="$candidate"; break; fi
done
for candidate in \
    /usr/share/OVMF/OVMF_VARS_4M.fd \
    /usr/share/edk2/ovmf/OVMF_VARS_4M.fd; do
    if [[ -f "$candidate" ]]; then ovmf_vars="$candidate"; break; fi
done
[[ -n "$ovmf_code" ]] || {
    echo "WIN-E04 OVMF_CODE_4M.fd was not found" >&2
    exit 1
}
[[ -n "$ovmf_vars" ]] || {
    echo "WIN-E05 OVMF_VARS_4M.fd was not found" >&2
    exit 1
}

install -m 0644 "$ovmf_code" "$payload_dir/OVMF_CODE_4M.fd"
install -m 0644 "$ovmf_vars" "$payload_dir/OVMF_VARS_4M.fd"

test -s "$esp_image"
test -s "$payload_dir/OVMF_CODE_4M.fd"
test -s "$payload_dir/OVMF_VARS_4M.fd"
echo "[windows] memory ESP: $esp_image"
echo "[windows] OVMF CODE: $payload_dir/OVMF_CODE_4M.fd"
echo "[windows] OVMF VARS: $payload_dir/OVMF_VARS_4M.fd"
