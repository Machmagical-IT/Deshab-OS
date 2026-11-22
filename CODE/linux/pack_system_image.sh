#!/usr/bin/env bash
#
# Pack SYSTEM/ into a GPT disk.
#
# SYSTEM/ 下只有三个分区目录，一个目录就是一个分区，打包器不再按白名单
# 分类：
#
#   SYSTEM/ESP   -> p1 FAT32 ESP  (EF00)   UEFI 固件与引导器读取的分区
#   SYSTEM/DATA  -> p2 ext4       (8300)   内核与系统数据分区
#   SYSTEM/WIN   -> p3 NTFS       (0700)   Windows Guest 系统卷
#
# 分区目录里的内容原样落到该分区根，目录层级不再被重新解释。哪份内容进
# 哪个分区，由源码树里的位置决定，打包器不再猜测。
#
# 整盘固定 DESHAB_IMAGE_SIZE_GIB（默认 25GiB），三个分区在这张盘里切开：
#   p1 ESP 7GiB，p2 DATA 3GiB，p3 拿走剩下的空间（约 15GiB）。
# 内容放不进就终止，不把盘子撑大。镜像写到更大的盘上时，首次启动的
# firstinit 按这三个分区的现有比例把 GPT 和文件系统一起扩满物理盘。
#
# 跨分区“同一份内容出现在两处”的关系写在 SYSTEM/LINKS.CNF（符号链接表）。
# 源树里只保留一份真实内容；打包器读表后在目标分区把该内容落实为硬链接
# （同一分区）或复制（跨分区——不同文件系统之间无法硬链接）。
#
# Usage:
#   sudo bash CODE/linux/pack_system_image.sh <SYSTEM_DIR> <OUTPUT_IMG>
#
# 三个分区一律创建。整盘默认 25GiB（DESHAB_IMAGE_SIZE_GIB）。
#
# FAT32 单文件上限 4GiB-1：ESP 分区内的文件必须小于该值。

set -Eeuo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <SYSTEM_DIR> <OUTPUT_IMG>" >&2
    exit 2
fi

SYSTEM="$(cd "$1" && pwd)"
OUT="$2"
OUT_DIR="$(cd "$(dirname "$OUT")" && pwd)"
OUT="$OUT_DIR/$(basename "$OUT")"

# Must match SYSTEM/ESP/limine/limine.conf uuid(...) paths.
EXT4_UUID="64657368-6162-4000-8000-000000000002"
DISK_GUID="64657368-6162-4000-8000-000000000001"
ESP_PARTGUID="64657368-6162-4000-8000-0000000000b1"
EXT4_PARTGUID="64657368-6162-4000-8000-0000000000e2"
WINDOWS_PARTGUID="64657368-6162-4000-8000-0000000000c3"

ESP_SRC="$SYSTEM/ESP"
DATA_SRC="$SYSTEM/DATA"
WIN_SRC="$SYSTEM/WIN"
LINK_TABLE="$SYSTEM/LINKS.CNF"

# Optional Windows Guest WIM. build.ps1 passes this when
# SYSTEM/windows/deshab-windows.wim is present.
WINDOWS_WIM="${DESHAB_WINDOWS_WIM:-}"
WINDOWS_INDEX="${DESHAB_WINDOWS_INDEX:-1}"
# 整盘尺寸。p1/p2 是固定切片，p3 = 整盘减去 GPT 开销和 p1/p2。
IMAGE_SIZE_GIB="${DESHAB_IMAGE_SIZE_GIB:-25}"
ESP_PART_GIB="${DESHAB_ESP_SIZE_GIB:-7}"
DATA_PART_GIB="${DESHAB_DATA_SIZE_GIB:-3}"

for tool in sfdisk mkfs.vfat mkfs.ext4 rsync losetup du; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[pack] ERROR: missing tool: $tool" >&2
        exit 2
    fi
done

if [[ -n "$WINDOWS_WIM" ]]; then
    for tool in mkfs.ntfs wimlib-imagex; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "[pack] ERROR: Windows Guest requires tool: $tool" >&2
            exit 2
        fi
    done
    if [[ ! -f "$WINDOWS_WIM" ]]; then
        echo "[pack] ERROR: Windows Guest WIM not found: $WINDOWS_WIM" >&2
        exit 2
    fi
    if [[ ! "$WINDOWS_INDEX" =~ ^[1-9][0-9]*$ ]]; then
        echo "[pack] ERROR: DESHAB_WINDOWS_INDEX must be a positive integer" >&2
        exit 2
    fi
fi

if [[ ! "$IMAGE_SIZE_GIB" =~ ^[1-9][0-9]*$ ]] || \
   [[ ! "$ESP_PART_GIB" =~ ^[1-9][0-9]*$ ]] || \
   [[ ! "$DATA_PART_GIB" =~ ^[1-9][0-9]*$ ]]; then
    echo "[pack] ERROR: DESHAB_IMAGE_SIZE_GIB / ESP / DATA sizes must be positive integers" >&2
    exit 2
fi
if (( ESP_PART_GIB + DATA_PART_GIB >= IMAGE_SIZE_GIB )); then
    echo "[pack] ERROR: ESP ${ESP_PART_GIB}GiB + DATA ${DATA_PART_GIB}GiB leave no room in a ${IMAGE_SIZE_GIB}GiB image" >&2
    exit 2
fi

# 三个分区目录必须存在（可以为空），这样“内容该放哪个分区”永远由目录
# 位置回答，而不是由打包器猜。缺目录 = 结构坏了，直接终止。
for d in "$ESP_SRC" "$DATA_SRC" "$WIN_SRC"; do
    if [[ ! -d "$d" ]]; then
        echo "[pack] ERROR: SYSTEM/ must contain ESP/, DATA/ and WIN/ (missing: $d)" >&2
        exit 2
    fi
done
if [[ ! -f "$ESP_SRC/limine/limine.conf" ]]; then
    echo "[pack] ERROR: SYSTEM/ESP/limine/limine.conf is required" >&2
    exit 2
fi

# 目录/文件表观大小（字节）。
#
# 必须容错：SYSTEM/WIN 是完整 Windows 安装，带着原系统 ACL —— Windows/Temp、
# SysWOW64/config 等目录在普通用户下 du 直接 Permission denied 并返回非零，
# 而脚本开了 set -e，会让整个打包中断。这里吞掉 stderr 并容忍退出码，
# 拿不到准确值就退化为 0（分区尺寸另有下限与 tar 大小兜底）。
bytes_of() {
    local p="$1"
    if [[ -e "$p" ]]; then
        local n
        n="$(du -sb --apparent-size "$p" 2>/dev/null | awk 'NR==1{print $1}')" || n=""
        if [[ -z "$n" ]]; then
            # du 读不全时用部分结果（--apparent-size 仍会累加可读部分）
            n=0
        fi
        echo "$n"
    else
        echo 0
    fi
}

align_up() {
    local val="$1"
    local align="$2"
    echo $(( (val + align - 1) / align * align ))
}

# 目录内容大小（不含目录自身条目）——空目录也算 0 字节内容。
dir_content_bytes() {
    local d="$1"
    local total=0
    local child
    shopt -s dotglob nullglob
    for child in "$d"/*; do
        total=$((total + $(bytes_of "$child")))
    done
    shopt -u dotglob nullglob
    echo "$total"
}

# ---- 符号链接表解析 -------------------------------------------------------
# 输出规范化后的记录：<DST_PART>\t<DST_PATH>\t<SRC_PART>\t<SRC_PATH>
# 表格式见 SYSTEM/LINKS.CNF 头部注释。
parse_link_table() {
    [[ -f "$LINK_TABLE" ]] || return 0
    local lineno=0 line
    while IFS= read -r line || [[ -n "$line" ]]; do
        lineno=$((lineno + 1))
        # 去注释与首尾空白
        line="${line%%#*}"
        line="$(printf '%s' "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
        [[ -z "$line" ]] && continue

        if [[ ! "$line" =~ ^([A-Za-z]+):(/[^[:space:]]*)[[:space:]]*\<-[[:space:]]*([A-Za-z]+):(/[^[:space:]]*)$ ]]; then
            echo "[pack] ERROR: $LINK_TABLE:$lineno: cannot parse link rule: $line" >&2
            echo "[pack]        expected: <PART>:/path <- <PART>:/path" >&2
            exit 1
        fi

        local dp="$(printf '%s' "${BASH_REMATCH[1]}" | tr '[:lower:]' '[:upper:]')"
        local dpath="${BASH_REMATCH[2]}"
        local sp="$(printf '%s' "${BASH_REMATCH[3]}" | tr '[:lower:]' '[:upper:]')"
        local spath="${BASH_REMATCH[4]}"

        case "$dp" in ESP|DATA|WIN) ;; *)
            echo "[pack] ERROR: $LINK_TABLE:$lineno: unknown destination partition '$dp'" >&2
            exit 1 ;;
        esac
        case "$sp" in ESP|DATA|WIN) ;; *)
            echo "[pack] ERROR: $LINK_TABLE:$lineno: unknown source partition '$sp'" >&2
            exit 1 ;;
        esac
        if [[ "$dp" == "$sp" && "$dpath" == "$spath" ]]; then
            echo "[pack] ERROR: $LINK_TABLE:$lineno: source and destination are identical" >&2
            exit 1
        fi

        printf '%s\t%s\t%s\t%s\n' "$dp" "$dpath" "$sp" "$spath"
    done < "$LINK_TABLE"
}

# ---- 分区内容大小 ---------------------------------------------------------
# 三个分区目录固定各对应一个分区，一律创建，不因“目录暂时为空”而少建分区。
# 分区表是运行期契约（fat32.drv 按 GPT 遍历、ext4.drv 认 p2、VMM 认 p3），
# 分区数量随内容变化会让引导器和驱动看到不同的磁盘形状。
esp_content="$(dir_content_bytes "$ESP_SRC")"
data_content="$(dir_content_bytes "$DATA_SRC")"
# 有 WIM/tar 时不必遍历 SYSTEM/WIN：既省时间，也避开 ACL 拒绝（du 读不全
# Windows/Temp 等目录）。它们是 p3 的权威输入，目录树只供宿主检查。
if [[ -n "$WINDOWS_WIM" || -n "${DESHAB_WIN_TAR:-}" ]]; then
    win_content=0
    echo "[pack]   (WIN measured from image payload; skipping SYSTEM/WIN walk)"
else
    win_content="$(dir_content_bytes "$WIN_SRC")"
fi

link_records="$(parse_link_table)"

if (( esp_content == 0 )); then
    echo "[pack] ERROR: SYSTEM/ESP is empty; a boot ESP is required" >&2
    exit 2
fi

# 链接表会让目标分区额外承载来源内容，分区容量必须把这部分算进去，
# 否则复制完成前就写爆（ext4/NTFS 会报 No space left）。
link_bytes_for() {
    local want_part="$1"
    local total=0 dp dpath sp spath
    [[ -n "$link_records" ]] || { echo 0; return; }
    while IFS=$'\t' read -r dp dpath sp spath; do
        [[ -z "$dp" || "$dp" != "$want_part" ]] && continue
        local src_root="$ESP_SRC"
        case "$sp" in
            ESP)  src_root="$ESP_SRC" ;;
            DATA) src_root="$DATA_SRC" ;;
            WIN)  src_root="$WIN_SRC" ;;
        esac
        local shost="$src_root$spath"
        [[ -e "$shost" ]] || continue
        total=$((total + $(bytes_of "$shost")))
    done <<< "$link_records"
    echo "$total"
}

esp_link_bytes="$(link_bytes_for ESP)"
data_link_bytes="$(link_bytes_for DATA)"
win_link_bytes="$(link_bytes_for WIN)"

# 链接表会让目标分区额外承载来源内容，分区容量必须把这部分算进去。
esp_content_total=$((esp_content + esp_link_bytes))
data_content_total=$((data_content + data_link_bytes))
win_content_total=$((win_content + win_link_bytes))

# --- 分区容量：整盘固定，切给三个分区 ------------------------------------
# 默认 25GiB 整盘：p1 7GiB、p2 3GiB，p3 拿走剩下的（含 GPT 前后各 1MiB）。
# 内容放不进自己的切片就终止。不把整盘撑过 IMAGE_SIZE_GIB。
MIB=$((1024 * 1024))
SECTOR=512
GIB=$((1024 * MIB))
gpt_front=2048
gpt_back=2048
image_bytes=$((IMAGE_SIZE_GIB * GIB))
gpt_overhead=$(( (gpt_front + gpt_back) * SECTOR ))
esp_bytes=$((ESP_PART_GIB * GIB))
data_bytes=$((DATA_PART_GIB * GIB))
win_bytes=$((image_bytes - gpt_overhead - esp_bytes - data_bytes))
if (( win_bytes < GIB )); then
    echo "[pack] ERROR: p3 would be under 1GiB in a ${IMAGE_SIZE_GIB}GiB image" >&2
    exit 2
fi

# 内容所需的下限（内容 + 15% + 64MiB 元数据余量）
esp_need=$((esp_content_total + esp_content_total / 7 + 64 * MIB))
data_need=$((data_content_total + data_content_total / 7 + 64 * MIB))
if (( esp_need > esp_bytes )); then
    echo "[pack] ERROR: p1 content needs $((esp_need / MIB)) MiB, slice is $((esp_bytes / MIB)) MiB" >&2
    exit 2
fi
if (( data_need > data_bytes )); then
    echo "[pack] ERROR: p2 content needs $((data_need / MIB)) MiB, slice is $((data_bytes / MIB)) MiB" >&2
    exit 2
fi

# p3 必须装得下 Windows 载荷。WIM 的 Total Bytes 把硬链接重复计算，
# 盘上占用是 Total Bytes - Hard Link Bytes（这张 WIM 约 12.7GiB，
# 落到 NTFS 上约 14GiB）。余量按 8% + 256MiB。tar 按 1.1 倍估；
# 目录按内容 + 15% + 1GiB。放不下就终止，不把整盘撑大。
win_need=0
if [[ -n "$WINDOWS_WIM" ]]; then
    wim_info="$(wimlib-imagex info "$WINDOWS_WIM" "$WINDOWS_INDEX" 2>/dev/null || true)"
    wim_total="$(printf '%s\n' "$wim_info" | awk -F: '/^Total Bytes:/ {gsub(/[^0-9]/,"",$2); print $2; exit}')"
    wim_links="$(printf '%s\n' "$wim_info" | awk -F: '/^Hard Link Bytes:/ {gsub(/[^0-9]/,"",$2); print $2; exit}')"
    if [[ -z "$wim_total" ]]; then
        echo "[pack] ERROR: cannot read WIM total bytes: $WINDOWS_WIM index $WINDOWS_INDEX" >&2
        exit 2
    fi
    wim_links="${wim_links:-0}"
    wim_bytes=$((wim_total - wim_links))
    if (( wim_bytes < 1 )); then
        wim_bytes="$wim_total"
    fi
    win_need=$((wim_bytes + wim_bytes / 12 + 256 * MIB))
elif [[ -n "${DESHAB_WIN_TAR:-}" ]]; then
    tar_bytes="$(bytes_of "$DESHAB_WIN_TAR")"
    win_need=$((tar_bytes + tar_bytes / 10))
elif (( win_content_total > 0 )); then
    win_need=$((win_content_total + win_content_total / 7 + 1024 * MIB))
fi
if (( win_need > win_bytes )); then
    echo "[pack] ERROR: p3 payload needs $((win_need / MIB)) MiB, slice is $((win_bytes / MIB)) MiB (image ${IMAGE_SIZE_GIB}GiB)" >&2
    exit 2
fi
esp_sectors=$((esp_bytes / SECTOR))
data_sectors=$((data_bytes / SECTOR))
win_sectors=$((win_bytes / SECTOR))
total_sectors=$((gpt_front + esp_sectors + data_sectors + win_sectors + gpt_back))
total_bytes=$((total_sectors * SECTOR))
if (( total_bytes != image_bytes )); then
    echo "[pack] ERROR: disk size $total_bytes != ${IMAGE_SIZE_GIB}GiB ($image_bytes)" >&2
    exit 2
fi

# FAT32 硬限制：单文件 < 4GiB。分区再大也放不下超限文件，必须在打包前拦截，
# 否则 mkfs/拷贝会在中途失败或产生不可读的文件。ESP 上主要风险是
# linux-extra-rootfs.img（持久化卷）与 linux-extra-rootfs 类大镜像。
# 注：持久化卷现在应放在 SYSTEM/DATA/persist/（ext4，无 4GiB 限制），
# 只有遗留布局才会出现在 ESP 上；出现时这里会拦截而非静默损坏。
FAT32_MAX_FILE=$((4 * 1024 * 1024 * 1024 - 1))
if [[ -d "$ESP_SRC" ]]; then
    while IFS= read -r -d '' bigf; do
        sz="$(stat -c '%s' "$bigf" 2>/dev/null || echo 0)"
        if (( sz > FAT32_MAX_FILE )); then
            echo "[pack] ERROR: file exceeds FAT32 4GiB limit on p1:" >&2
            echo "[pack]        $bigf ($((sz / 1024 / 1024)) MiB)" >&2
            echo "[pack]        move it to SYSTEM/DATA, or shrink it before packing" >&2
            exit 2
        fi
    done < <(find "$ESP_SRC" -type f -size +4G -print0 2>/dev/null)
fi

echo "[pack] GPT partition layout (one SYSTEM/ subdir == one partition):"
echo "[pack]   p1 FAT32 ESP  <- SYSTEM/ESP   $((esp_content / 1024 / 1024)) MB content / $((esp_bytes / 1024 / 1024)) MB partition"
echo "[pack]   p2 ext4       <- SYSTEM/DATA  $((data_content / 1024 / 1024)) MB content / $((data_bytes / 1024 / 1024)) MB partition  uuid=$EXT4_UUID"
if [[ -n "$WINDOWS_WIM" ]]; then
    echo "[pack]   p3 NTFS       <- SYSTEM/WIN + Windows WIM index=$WINDOWS_INDEX  partition=$((win_bytes / 1024 / 1024)) MB"
else
    echo "[pack]   p3 NTFS       <- SYSTEM/WIN   $((win_content / 1024 / 1024)) MB content / $((win_bytes / 1024 / 1024)) MB partition"
fi
if [[ -n "$link_records" ]]; then
    echo "[pack]   cross-partition links from SYSTEM/LINKS.CNF:"
    while IFS=$'\t' read -r dp dpath sp spath; do
        [[ -z "$dp" ]] && continue
        echo "[pack]     $dp:$dpath <- $sp:$spath"
    done <<< "$link_records"
fi
echo "[pack] SYSTEM=$SYSTEM"
echo "[pack] disk=$((total_bytes / 1024 / 1024)) MB  -> $OUT"

# 中间镜像放在 WSL 本地盘：104GiB 的稀疏文件经 9p 写回 D:\ 之前先在本地
# 建好。/var/tmp 在 WSL ext4 上，当前有数百 GiB 空闲。
WORK="$(mktemp -d /var/tmp/deshab_pack.XXXXXX)"
IMG="$WORK/disk.img"
MNT_ESP="$WORK/mnt_esp"
MNT_DATA="$WORK/mnt_data"
MNT_WIN="$WORK/mnt_win"
LOOP=""

cleanup() {
    set +e
    if [[ -n "${MNT_ESP:-}" ]]; then
        umount "$MNT_ESP" 2>/dev/null
    fi
    if [[ -n "${MNT_DATA:-}" ]]; then
        umount "$MNT_DATA" 2>/dev/null
    fi
    if [[ -n "${MNT_WIN:-}" ]]; then
        umount "$MNT_WIN" 2>/dev/null
    fi
    if [[ -n "${LOOP:-}" ]]; then
        losetup -d "$LOOP" 2>/dev/null
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$MNT_ESP" "$MNT_DATA" "$MNT_WIN"
# Prefer a sparse image so the 9p copy back to Windows is smaller when possible.
truncate -s "$total_bytes" "$IMG"

esp_start="$gpt_front"
data_start=$((esp_start + esp_sectors))
win_start=$((data_start + data_sectors))

{
cat <<EOF
label: gpt
label-id: ${DISK_GUID}
first-lba: ${gpt_front}
sector-size: 512

start=${esp_start}, size=${esp_sectors}, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=${ESP_PARTGUID}, name=DESHABBOOT
start=${data_start}, size=${data_sectors}, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=${EXT4_PARTGUID}, name=DESHAB
start=${win_start}, size=${win_sectors}, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, uuid=${WINDOWS_PARTGUID}, name=DESHABWIN
EOF
} | sfdisk --no-reread "$IMG"

LOOP="$(losetup --find --show --partscan "$IMG")"
echo "[pack] loop=$LOOP"
echo "[pack] partition table:"
sfdisk -l "$IMG" || true

p1=""
p2=""
p3=""
for _ in $(seq 1 25); do
    if [[ -b "${LOOP}p1" && -b "${LOOP}p2" && -b "${LOOP}p3" ]]; then
        p1="${LOOP}p1"
        p2="${LOOP}p2"
        p3="${LOOP}p3"
        break
    fi
    # Some kernels expose /dev/loopXp1 only after partx.
    if command -v partx >/dev/null 2>&1; then
        partx --update "$LOOP" 2>/dev/null || true
    fi
    sleep 0.2
done
if [[ -z "$p1" || -z "$p2" || -z "$p3" ]]; then
    echo "[pack] ERROR: loop partitions did not appear under $LOOP" >&2
    ls -l "$LOOP"* 2>/dev/null || true
    exit 1
fi

# ESP 的簇大小必须显式指定，不能交给 mkfs 自动选。
#
# 历史坑一（OVMF 挂不上）：曾经写死 -s 8（4KiB 簇）。256MiB 的 ESP 在该几何下
# 恰好得到 65535 个簇 —— 正是 FAT32 的 0xFFF5 边界 —— OVMF(EDK2 FatPkg) 拒挂
# 该卷，现象是 BdsDxe "Not Found"，Shell 里分区只有 BLKn 没有 FS。
#
# 历史坑二（根目录写满）：修坑一时改成不指定 -s 交给 mkfs 自动选，它给出
# spc=1（512B/簇）。根目录每簇只能放 16 个目录项，而打包进 ESP 根目录的内容
# （DESHABBOOT/EFI/LIMINE/BOOT/DRIVER/SYSTEM/USER/NVVARS + 它们的 LFN 项）
# 正好 16 项，根目录链又只有单簇 —— 于是首次启动写 USER.CONF 时必然
# 拿到 "no free dir entry"（-4）→ FS-E02 panic，系统进不去。
#
# 现在分区固定 7GiB（镜像内的切片；实机首次启动再按比例扩张），簇数的
# 量级和当年 256MiB 完全不同，两个历史坑都不会再出现，但有一条硬约束：
#
#   DKM fat32.drv 的 g_fat32_cluster 是 64KiB（FAT32 每簇扇区数的上限）。
#   mkfs 在 12G 上自动选 spc=16（8KiB），32G 上选 spc=32（16KiB），
#   64G 以上会到 32KiB/64KiB。镜像仍固定 2KiB；firstinit 扩到更大盘时再加大簇。
#
# 12GiB 各档实测（-s 4 / auto / 64）：
#   -s 4   2KiB/簇  6266957 簇  FAT 24482KiB  根目录 64 项/簇
#   auto   8KiB/簇  1571323 簇  FAT  6144KiB  根目录 256 项/簇
#   -s 64  32KiB/簇  393118 簇  FAT  1536KiB  根目录 1024 项/簇
#
# 固定 -s 4 的理由：簇最小 → 浪费最少（ESP 装的是零散小文件），根目录 64 项/簇
# 仍有余量，且 2KiB 远低于 64KiB 缓冲上限。7GiB 上两份 FAT 大约 28MiB。
# firstinit 在簇号要超出 28 位时把簇成倍加大（最大 64KiB）。FAT32 总扇区
# 字段是 32 位，卷本身最大 2TiB。
#
# 注意：ESP 总扇区数 = 7GiB/512 = 14680064，远小于 u32 上限，扇区号运算安全。
mkfs.vfat -F 32 -s 4 -n DESHABBOOT "$p1" >/dev/null
# ext4 读写驱动支持范围：4K block / 256B inode / extent 树，
# 不支持 journal、metadata_csum、64bit（遇到即 panic EXT4-E2x）。
# resize_inode 是 compat 特性，驱动忽略它。有预留块时 firstinit 直接往里写
# GDT；预留不够时把挡住的块搬到空闲区。没有 64bit，块数到 2^32 为止。
mkfs.ext4 -F -q -L DESHAB -U "$EXT4_UUID" -m 1 \
    -b 4096 -I 256 -O ^has_journal,^metadata_csum,^64bit,resize_inode "$p2"
# p3 永远创建。有 Windows WIM 时它是 Guest 系统卷；否则是 SYSTEM/WIN 的
# 普通 NTFS 数据卷，内容原样落盘。两种情形都只有一个持久 Windows 分区，
# UEFI 启动资源仍是 SYSTEM/ESP/boot 里的 capsule，由 VMM 在内存中暴露。
mkfs.ntfs -F -Q -L DESHABWIN -p "$win_start" -H 255 -S 63 "$p3" >/dev/null
echo "[pack] filesystems:"
blkid "$p1" "$p2" "$p3" || true

mount "$p1" "$MNT_ESP"
mount "$p2" "$MNT_DATA"
mount -t ntfs3 "$p3" "$MNT_WIN" 2>/dev/null || mount -t ntfs-3g "$p3" "$MNT_WIN"

# ---- p1 FAT32 ESP：SYSTEM/ESP 原样 --------------------------------------
# 9p 上大文件吞吐约 93MB/s，但逐文件 stat（rsync 的 -t/-l 需要）代价高，
# ESP 里混着 1GB/3.9GB 大文件和上千小文件，走 -rltD 会慢到十几分钟。
# 这里用 cp -r 保留内容（FAT32 没有 Unix 权限/时间戳语义，无需 -a），
# 只在大文件传输上付出线性代价。
echo "[pack] copying p1 FAT32 ESP <- SYSTEM/ESP"
cp -r "$ESP_SRC"/. "$MNT_ESP"/

# limine.conf 的 Linux 兼容层 module_path 段必须与 SYSTEM/ESP/boot/ 的实际产物一致。
# CODE/linux/build.sh 建完 Linux 内核会自动取消那几行注释，但没有任何地方会关回去；
# 而 055c521 之后 .drv / Linux 产物都不入库，于是「注释开着但文件不存在」会让
# Limine 直接 panic（Failed to open module ...），系统根本进不去。
# 这里按实际存在的文件过滤后再写进 ESP —— 源文件 SYSTEM/ESP/limine/limine.conf 保持不动。
#
# 注意：Limine 10 优先读 ESP 上的 /limine/limine.conf（目录形式），其次才是
# 根目录 /limine.conf。上面 rsync 已经把未过滤的原版拷进 /limine/ 了，所以
# 两处都必须写过滤后的副本，否则改根目录那份不起作用。
filter_limine_conf() {
    local dst="$1"
    python3 - "$ESP_SRC/limine/limine.conf" "$ESP_SRC/boot" "$dst" <<'PYEOF'
import os, re, sys

src, bootdir, dst = sys.argv[1], sys.argv[2], sys.argv[3]
# Linux/guest 内核段的 module_path（其它 module_path 靠路径前缀区分）
GUEST_PREFIXES = ('/boot/linux-', '/boot/xj380.krl')

lines = open(src, 'r', encoding='utf-8').read().splitlines(keepends=True)
out, dropped, kept = [], [], []

for line in lines:
    m = re.match(r'^(\s*)module_path:\s*boot\(\):(\S+)\s*$', line)
    if m and m.group(2).startswith(GUEST_PREFIXES):
        host = os.path.join(bootdir, m.group(2).split('/boot/', 1)[1])
        if os.path.exists(host):
            kept.append(m.group(2))
            out.append(line)
        else:
            # 产物不存在 → 注释掉，避免 Limine 加载失败 panic
            out.append('%s# [pack] module missing on disk, disabled: %s' % (m.group(1), line))
            dropped.append(m.group(2))
    else:
        out.append(line)

open(dst, 'w', encoding='utf-8').write(''.join(out))
for p in kept:
    print('[pack]   limine module kept: %s' % p)
for p in dropped:
    print('[pack]   limine module DISABLED (file absent): %s' % p)
PYEOF
}

if [[ -f "$ESP_SRC/limine/limine.conf" ]]; then
    mkdir -p "$MNT_ESP/limine"
    filter_limine_conf "$MNT_ESP/limine.conf"        # 根目录副本
    filter_limine_conf "$MNT_ESP/limine/limine.conf" # Limine 10 实际优先读的那份
fi

# ---- p2 ext4 DATA：SYSTEM/DATA 原样 ------------------------------------
echo "[pack] copying p2 ext4 DATA <- SYSTEM/DATA"
rsync -a --info=progress2 "$DATA_SRC/" "$MNT_DATA/"

# ---- p3 NTFS WIN：SYSTEM/WIN -------------------------------------------
# 三种来源，按优先级：
#   1. DESHAB_WINDOWS_WIM   — 已有 WIM，wimlib 直接 apply（保 ACL/硬链接/重解析点）
#   2. DESHAB_WIN_TAR       — Windows 侧（提权）打好的 tar，这里解包
#   3. 直接 rsync SYSTEM/WIN
#
# 为什么需要 (2)：SYSTEM/WIN 是完整 Windows 安装（数万文件 + 原系统 ACL，
# 含 PerfLogs / $Recycle.Bin 等普通用户读不了的目录）。从 WSL 经 9p 逐文件
# 读 D:\ 会既慢又报 "file has vanished"；提权后在 Windows 侧打成单个 tar
# 流，WSL 只读一个文件，绕开这两个问题。
echo "[pack] copying p3 NTFS WIN <- SYSTEM/WIN"
if [[ -n "$WINDOWS_WIM" ]]; then
    echo "[pack] applying Windows WIM index $WINDOWS_INDEX to p3"
    wimlib-imagex apply "$WINDOWS_WIM" "$WINDOWS_INDEX" "$MNT_WIN" \
        --no-acls --no-attributes --rpfix --quiet
elif [[ -n "${DESHAB_WIN_TAR:-}" ]]; then
    if [[ ! -f "$DESHAB_WIN_TAR" ]]; then
        echo "[pack] ERROR: DESHAB_WIN_TAR not found: $DESHAB_WIN_TAR" >&2
        exit 1
    fi
    echo "[pack] extracting WIN tar into p3: $DESHAB_WIN_TAR"
    # tar 里路径是相对的（./Windows/...），直接解到 p3 根。
    # 用 --no-same-owner/--no-same-permissions：源是 Windows ACL，目标 NTFS
    # 由内核处理权限，保留 Unix 位没有意义。
    tar -xf "$DESHAB_WIN_TAR" -C "$MNT_WIN" --no-same-owner --no-same-permissions
else
    # 纯数据卷：容忍源文件在传输中消失（rsync 24），Windows 目录树里
    # 常有正在被系统改写的项，不应因此中断整个镜像构建。
    set +e
    rsync -a --info=progress2 "$WIN_SRC/" "$MNT_WIN/"
    rsync_rc=$?
    set -e
    if [[ $rsync_rc -ne 0 && $rsync_rc -ne 24 ]]; then
        echo "[pack] ERROR: rsync WIN failed ($rsync_rc)" >&2
        exit 1
    fi
    [[ $rsync_rc -eq 24 ]] && echo "[pack]   (rsync reported vanished source files; continuing)"
fi

# ---- SYSTEM/LINKS.CNF：符号链接表 --------------------------------------
# 表项声明“目标分区这个位置应当出现与来源相同的内容”，而源树里只写一份。
#
#   ESP:/driver <- DATA:/driver   → p1 的 /driver 复刻 DATA/driver 的内容
#
# 同一文件系统内用硬链接（零额外空间）；跨文件系统（FAT32 ↔ ext4 ↔ NTFS）
# 内核不允许硬链接，退化为复制。运行期看到的是真实文件，不依赖任何挂载
# 顺序或路径解析。
mnt_for_part() {
    case "$1" in
        ESP)  echo "$MNT_ESP" ;;
        DATA) echo "$MNT_DATA" ;;
        WIN)  echo "$MNT_WIN" ;;
        *)    echo "" ;;
    esac
}

# 判断两个目标路径是否在同一文件系统上（st_dev 相同才能硬链接）。
same_fs() {
    stat -c '%d' "$1" 2>/dev/null || echo "x"
}

if [[ -n "$link_records" ]]; then
    echo "[pack] applying SYSTEM/LINKS.CNF"
    while IFS=$'\t' read -r dp dpath sp spath; do
        [[ -z "$dp" ]] && continue

        dmnt="$(mnt_for_part "$dp")"
        smnt="$(mnt_for_part "$sp")"
        if [[ -z "$dmnt" ]]; then
            echo "[pack] ERROR: destination partition $dp is not present in this image" >&2
            exit 1
        fi

        dhost="$dmnt$dpath"
        shost="$smnt$spath"

        if [[ ! -e "$shost" ]]; then
            echo "[pack] ERROR: LINKS.CNF source is missing: $sp:$spath ($shost)" >&2
            exit 1
        fi

        # 目标已存在：表项是“应当出现”的声明，已存在真实数据即视为结构漂移。
        if [[ -L "$dhost" ]]; then
            rm -f "$dhost"
        elif [[ -e "$dhost" ]]; then
            echo "[pack] ERROR: LINKS.CNF destination already holds real data: $dp:$dpath" >&2
            echo "[pack]        keep the content in one place only (source: $sp:$spath)" >&2
            exit 1
        fi

        mkdir -p "$(dirname "$dhost")"

        if [[ -f "$shost" ]] && [[ ! -d "$shost" ]] &&
           [[ "$(same_fs "$shost")" == "$(same_fs "$(dirname "$dhost")")" ]]; then
            ln "$shost" "$dhost"
            echo "[pack]   $dp:$dpath <= hardlink $sp:$spath"
        else
            if [[ -d "$shost" ]]; then
                rsync -a "$shost/" "$dhost/"
            else
                cp -f "$shost" "$dhost"
            fi
            echo "[pack]   $dp:$dpath <= copy $sp:$spath ($(( $(bytes_of "$shost") / 1024 )) KB)"
        fi
    done <<< "$link_records"
fi

sync
umount "$MNT_ESP"
umount "$MNT_DATA"
umount "$MNT_WIN"
losetup -d "$LOOP"
LOOP=""

echo "[pack] writing $OUT"
mkdir -p "$OUT_DIR"
# Sparse copy keeps the Windows-side file smaller when the host supports it.
cp --sparse=always "$IMG" "$OUT"
sync

echo "[pack] done: $OUT"
echo "[pack]   p1 FAT32 ESP  (SYSTEM/ESP)"
echo "[pack]   p2 ext4 DATA  (SYSTEM/DATA)  uuid=$EXT4_UUID"
echo "[pack]   p3 NTFS WIN   (SYSTEM/WIN)"
