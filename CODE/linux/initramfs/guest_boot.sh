#!/bin/bash
# ============================================================================
# guest_boot.sh — Linux guest 的 PID 1 启动 wrapper
#
# 这份脚本是 Arch rootfs 里的 /usr/local/bin/utsm_exec_daemon（wrapper 形式）。
# 它在把 vCPU park 给 UTSM 之前做三件事：
#   1. 起 tmpfs（/tmp、/run）
#   2. 铺 pacman 可写层：/usr、/etc、/var 各挂一层 overlay
#      （upper 优先落在 /dev/vdc 持久卷，挂不上退回 tmpfs）
#   3. 拉起网络（eth0 uplink + mac80211_hwsim 演示 AP）
# 最后 exec 真正的 daemon（utsm_exec_daemon.real），它保持 PID 1。
#
# 为什么需要可写层：rootfs 是只读 ext4（virtio-blk 写入会弄脏镜像，之后
# iget 校验失败）。只读状态下 pacman 只能 -Sy/查询，装包必然失败在写 /usr。
# overlay 让 /usr /etc /var 可写而镜像本身不动。
#
# 持久化：overlay 的 upper 放在 /dev/vdc（deshab-persist ext4，写入经
# virtio-blk 回写 ESP 上的 linux-extra-rootfs.img），因此装的包在 guest
# 重启后仍在。vdc 挂不上时自动退回 tmpfs（可用但重启即失）。
#
# 为什么需要 pacman-local 母本：pacman -S 判断文件冲突靠
# /var/lib/pacman/local（本地已装包清单）。镜像里那份在只读区，启动时复制
# 到可写位置。少了它 pacman 会认为系统是空的，安装 base 系依赖报
# "exists in filesystem"。
#
# %DAEMON% 由构建/补丁脚本替换为真 daemon 路径。
# ============================================================================
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

mount -t tmpfs none /tmp 2>/dev/null
mount -t tmpfs none /run 2>/dev/null
mkdir -p /tmp/pacman/lib/sync /tmp/pacman/cache/pkg 2>/dev/null

# ---------- pacman local 库：只读母本 → 可写位置 ----------
PRISTINE_DB=/usr/local/share/deshab/pacman-local

# ---------- 挑选 overlay 的 upper 落点 ----------
# 首选 /dev/vdc（extra-rootfs）：guest 重启后仍在 —— 这才是「装的包不丢」。
# 挂不上才退回 tmpfs（此时一切仍可用，只是重启即失）。
#
# 卷的实体现在位于 DATA 分区（ext4）的 /persist/deshab-persist.img：
#   UTSM 侧 ext4_blkmap 只读超级块/inode/extent 元数据，把文件映射成扇区表，
#   virtio-blk 把它当作 /dev/vdc 暴露给本 guest。ext4 没有 FAT32 的 4GiB
#   单文件限制，卷可以按需做大。
#   （遗留布局仍支持：ESP 上的 linux-extra-rootfs.img 走 FAT32 簇链映射。）
#
# 已知陷阱：卷内部是 `mkfs.ext4 -O ^has_journal` 建的（无日志）且开
# metadata_csum。无日志的 ext4 在写事务中途掉电/复位会让 inode 校验和不符，
# 内核随后拒绝该 inode（"iget checksums fail" 的来源）。所以挂载前先 fsck -p：
# 干净盘不动，被打断的盘修好。
RW_BASE=""
# 挂载点必须可写：guest 根是只读 ext4，/mnt 在只读层上，mkdir 会失败。
# /tmp 是本脚本刚起的 tmpfs，一定可写，所以把 vdc 挂到 /tmp/persist。
# （挂载点只是"入口"，真正的数据在 /dev/vdc 的 ext4 上，重启仍在。）
PERSIST_MNT=/tmp/persist
if [ -b /dev/vdc ]; then
    mkdir -p "$PERSIST_MNT"
    # vdc 是 ^has_journal 的 ext4 + metadata_csum：无日志时写事务被打断会让
    # inode 校验和不符，内核随后拒绝该 inode（"iget checksums fail" 的来源）。
    # 每次挂载前 fsck -p：干净盘不动，被打断的盘修好。
    fsck.ext4 -p /dev/vdc >/tmp/vdc-fsck.log 2>&1
    if mount -t ext4 /dev/vdc "$PERSIST_MNT" 2>/tmp/vdc-mount.log; then
        mkdir -p "$PERSIST_MNT/deshab-rw"
        RW_BASE="$PERSIST_MNT/deshab-rw"
        echo "[deshab-rw] persistent upper on /dev/vdc -> $RW_BASE"
        # 持久卷快满时 pacman 会以 "transaction aborted" 收场，且错误信息
        # 不点明是空间问题。这里提前把剩余空间打出来，便于定位。
        avail_kb=$(df -Pk "$PERSIST_MNT" 2>/dev/null | awk 'NR==2{print $4}')
        if [ -n "${avail_kb:-}" ]; then
            avail_mb=$((avail_kb / 1024))
            echo "[deshab-rw] persist free=${avail_mb}MB"
            if [ "$avail_mb" -lt 512 ]; then
                echo "[deshab-rw] WARNING: persistent volume nearly full; pacman -S may fail"
            fi
        fi
    else
        echo "[deshab-rw] WARNING: /dev/vdc mount failed — falling back to tmpfs"
        cat /tmp/vdc-mount.log 2>/dev/null
    fi
fi
if [ -z "$RW_BASE" ]; then
    RW_BASE=/tmp/deshab-rw
    echo "[deshab-rw] WARNING: no persistent volume — installs will not survive reboot"
fi
echo "[deshab-rw] RW_BASE=$RW_BASE"

# ---------- 可写层：/usr /etc /var 各一层 overlay ----------
# 必须在 /tmp 这个 tmpfs 就绪之后挂（tmpfs 兜底时 upperdir 在 /tmp 下）。
for d in usr etc var; do
    mkdir -p "$RW_BASE/$d/upper" "$RW_BASE/$d/work" 2>/dev/null
    if mount -t overlay overlay \
         -o "lowerdir=/$d,upperdir=$RW_BASE/$d/upper,workdir=$RW_BASE/$d/work" \
         "/$d" 2>/dev/null; then
        echo "[deshab-rw] /$d writable (overlay, upper=$RW_BASE/$d)"
    else
        echo "[deshab-rw] WARNING: /$d overlay failed — pacman install will fail"
    fi
done

# ---------- pacman 的 local 库 ----------
# 镜像里 /var/lib/pacman 是指向 /tmp/pacman/lib 的符号链接（只读时代的临时
# 做法）。现在 /var 已经挂在 overlay 上，可以把该符号链接替换成真实目录，
# pacman 的已装包清单就落进 upper —— vdc 持久、tmpfs 会话内有效，两种情况
# 都正确，且不需要额外的复制/回写逻辑。
if [ -L /var/lib/pacman ]; then
    rm -f /var/lib/pacman
fi
if [ ! -d /var/lib/pacman/local ]; then
    mkdir -p /var/lib/pacman
    if [ -d "$PRISTINE_DB" ]; then
        cp -a "$PRISTINE_DB" /var/lib/pacman/local
        echo "[deshab-rw] pacman local db seeded from pristine: $(ls /var/lib/pacman/local | wc -l) pkgs"
    else
        echo "[deshab-rw] WARNING: pristine pacman db missing — installs will report file conflicts"
    fi
else
    echo "[deshab-rw] pacman local db loaded (persisted): $(ls /var/lib/pacman/local | wc -l) pkgs"
fi
if [ -L /var/cache/pacman ]; then
    rm -f /var/cache/pacman
fi
mkdir -p /var/cache/pacman/pkg /var/log


# ---------- 网络 ----------
# virtio-net 桥到 QEMU e1000 -> slirp，网段固定 10.0.2.0/24。
ip link set lo up 2>/dev/null
if ip link set eth0 up 2>/dev/null; then
    ip addr add 10.0.2.15/24 dev eth0 2>/dev/null
    ip route add default via 10.0.2.2 dev eth0 2>/dev/null
    echo "nameserver 10.0.2.3" > /etc/resolv.conf 2>/dev/null
    echo "[deshab-net] eth0 up 10.0.2.15/24 gw 10.0.2.2"
fi

# ---------- 演示 AP（mac80211_hwsim wlan1）----------
# 开放 AP：用最小 nl80211 工具（静态，零 OpenSSL）。hostapd/curl 等链接
# libcrypto.so.3 的程序在本 VMM(L2) 下段错误/挂起。
rfkill unblock all 2>/dev/null
ip link set wlan0 up 2>/dev/null
if [ -x /usr/local/bin/deshab_ap ]; then
    ip link set wlan1 down 2>/dev/null
    iw dev wlan1 set type __ap 2>/dev/null
    ip link set wlan1 up 2>/dev/null
    /usr/local/bin/deshab_ap wlan1 Deshab-Net 2412 >/tmp/ap.log 2>&1 &
    echo "[deshab-net] deshab_ap launched (SSID Deshab-Net)"
    n=0
    while [ $n -lt 8 ]; do
        sleep 2
        iw dev wlan0 scan > /tmp/deshab-wifi.txt 2>/dev/null
        b=$(grep -ac 'BSS ' /tmp/deshab-wifi.txt 2>/dev/null)
        [ "${b:-0}" -gt 0 ] && break
        n=$((n+1))
    done
    echo "[deshab-net] scan bss=${b:-0} tries=$n"
fi

exec %DAEMON%
