#!/bin/bash
#
# Build Arch Linux rootfs ext4 image for UTSM Linux guest.
#
# Downloads the Arch Linux bootstrap rootfs, installs pacman with Chinese
# mirror configuration, bakes in utsm_exec_daemon, and packs everything
# into an ext4 image exposed to the Linux guest via virtio-blk (/dev/vdb).
#
# Output:
#   SYSTEM/ESP/boot/linux-rootfs.img  (ext4 image, ~512MB)
#
# Performance note:
#   All intermediate work (extraction, ext4 build) happens on the native
#   Linux filesystem (WORK_DIR) — NOT on /mnt/d (9p). The 9p mount is
#   catastrophically slow for thousands of small files. Only the final
#   image is copied to the Windows side.
#
# Prerequisites:
#   - WSL or native Linux with root access
#   - curl, mkfs.ext4, losetup, mount, tar (with zstd), rsync, cc

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_DIR="$PROJECT_ROOT/SYSTEM/ESP/boot"
OUTPUT_IMG="$OUTPUT_DIR/linux-rootfs.img"
# 1024MB leaves room for distro CPython + WiFi userspace (iw/wpa_supplicant/hostapd)
# on the virtio-blk rootfs. (Was 768MB before the WiFi backend added tools.)
IMG_SIZE_MB=${IMG_SIZE_MB:-1024}
ARCH_INSTALL_PYTHON="${ARCH_INSTALL_PYTHON:-1}"

# Work on native Linux FS for speed (NOT /mnt/d which is 9p — extremely slow).
WORK_DIR="${ARCH_ROOTFS_WORK:-/root/arch_rootfs_work}"
ROOTFS_DIR="$WORK_DIR/rootfs"
OLD_CACHE="$SCRIPT_DIR/.arch_rootfs_work/archlinux-bootstrap.tar.zst"

echo "[arch-rootfs] Building Arch Linux rootfs image"
echo "[arch-rootfs] Work dir: $WORK_DIR (native FS)"
echo "[arch-rootfs] Output:   $OUTPUT_IMG (${IMG_SIZE_MB}MB)"

mkdir -p "$WORK_DIR" "$ROOTFS_DIR" "$OUTPUT_DIR"

# 1. Download Arch bootstrap rootfs if not cached (.tar.zst format)
BOOTSTRAP_TAR="$WORK_DIR/archlinux-bootstrap.tar.zst"

# Migrate old cache from /mnt/d if present (saves re-download)
if [ ! -f "$BOOTSTRAP_TAR" ] && [ -f "$OLD_CACHE" ]; then
    echo "[arch-rootfs] Migrating cached tarball from /mnt/d → native FS..."
    cp "$OLD_CACHE" "$BOOTSTRAP_TAR"
    rm -rf "$SCRIPT_DIR/.arch_rootfs_work"
fi

download_bootstrap() {
    local url="$1"
    echo "[arch-rootfs] Trying: $url"
    curl -fL --retry 3 --connect-timeout 20 -o "$BOOTSTRAP_TAR" "$url"
}

if [ ! -f "$BOOTSTRAP_TAR" ]; then
    echo "[arch-rootfs] Downloading Arch bootstrap rootfs (.tar.zst)..."
    download_bootstrap "https://mirrors.tuna.tsinghua.edu.cn/archlinux/iso/latest/archlinux-bootstrap-x86_64.tar.zst" || \
    download_bootstrap "https://mirrors.ustc.edu.cn/archlinux/iso/latest/archlinux-bootstrap-x86_64.tar.zst" || \
    download_bootstrap "https://mirrors.aliyun.com/archlinux/iso/latest/archlinux-bootstrap-x86_64.tar.zst" || \
    download_bootstrap "https://mirrors.cloud.tencent.com/archlinux/iso/latest/archlinux-bootstrap-x86_64.tar.zst" || {
        echo "[arch-rootfs] ERROR: Failed to download bootstrap rootfs from all mirrors"
        exit 1
    }
fi
echo "[arch-rootfs] Bootstrap tarball: $(du -h "$BOOTSTRAP_TAR" | cut -f1)"

# 2. Extract bootstrap rootfs directly to ROOTFS_DIR (--strip-components=1
#    drops the root.x86_64/ wrapper, eliminating a costly rsync copy).
if [ ! -f "$ROOTFS_DIR/bin/bash" ]; then
    echo "[arch-rootfs] Extracting bootstrap rootfs (native FS, --strip-components=1)..."
    rm -rf "$ROOTFS_DIR"
    mkdir -p "$ROOTFS_DIR"
    tar --zstd -xf "$BOOTSTRAP_TAR" --strip-components=1 -C "$ROOTFS_DIR"
fi

if [ ! -f "$ROOTFS_DIR/bin/bash" ]; then
    echo "[arch-rootfs] ERROR: Extraction failed, /bin/bash not found"
    exit 1
fi
echo "[arch-rootfs] Rootfs extracted: $(du -sh "$ROOTFS_DIR" | cut -f1)"

# 3. Configure pacman mirrors (Chinese mirrors for faster access)
echo "[arch-rootfs] Configuring pacman mirrors..."
mkdir -p "$ROOTFS_DIR/etc/pacman.d"
cat > "$ROOTFS_DIR/etc/pacman.d/mirrorlist" <<'MIRRORS'
# China mirrors (sorted by speed)
Server = https://mirrors.tuna.tsinghua.edu.cn/archlinux/$repo/os/$arch
Server = https://mirrors.ustc.edu.cn/archlinux/$repo/os/$arch
Server = https://mirrors.aliyun.com/archlinux/$repo/os/$arch
Server = https://mirrors.cloud.tencent.com/archlinux/$repo/os/$arch
Server = https://mirrors.huaweicloud.com/archlinux/$repo/os/$arch
MIRRORS

# 4. Ensure pacman config exists (always rewrite: SigLevel=Never avoids
#    gnupg keyring writes on the read-only rootfs — pacman -Sy then works
#    without pacman-key init).
cat > "$ROOTFS_DIR/etc/pacman.conf" <<'PACMANCONF'
[options]
HoldPkg = pacman glibc
Architecture = auto
SigLevel = Never
LocalFileSigLevel = Never

[core]
Include = /etc/pacman.d/mirrorlist

[extra]
Include = /etc/pacman.d/mirrorlist
PACMANCONF

# 5. Create essential directories and device nodes
echo "[arch-rootfs] Creating device nodes and directories..."
mkdir -p "$ROOTFS_DIR"/{dev,proc,sys,tmp,run,mnt,root,home}
# var/lib/pacman 在重复构建时可能已是 5.6 留下的 symlink（指向 /tmp/pacman/lib），
# mkdir -p 会因此失败；这里容错，symlink 依旧可用（构建期会建好其目标目录）。
mkdir -p "$ROOTFS_DIR/etc" "$ROOTFS_DIR/var/log" "$ROOTFS_DIR/var/lib/pacman" || true

mknod -m 600 "$ROOTFS_DIR/dev/console" c 5 1 2>/dev/null || true
mknod -m 666 "$ROOTFS_DIR/dev/null" c 1 3 2>/dev/null || true
mknod -m 666 "$ROOTFS_DIR/dev/zero" c 1 5 2>/dev/null || true
mknod -m 666 "$ROOTFS_DIR/dev/ttyS0" c 4 64 2>/dev/null || true

# 5.5 Strip unnecessary files to reduce image size (~597M → ~300M).
#     The rootfs is a RAM-resident boot module, so every MB counts.
#     Removes: man pages, docs, info, dev headers, static libs, non-en/zh locales.
echo "[arch-rootfs] Stripping non-essential files..."
echo "[arch-rootfs]   before: $(du -sh "$ROOTFS_DIR" | cut -f1)"
rm -rf "$ROOTFS_DIR/usr/share/man" "$ROOTFS_DIR/usr/share/doc" \
       "$ROOTFS_DIR/usr/share/info" "$ROOTFS_DIR/usr/share/gtk-doc" \
       "$ROOTFS_DIR/usr/share/help" "$ROOTFS_DIR/usr/include" \
       "$ROOTFS_DIR/usr/share/readline" "$ROOTFS_DIR/usr/share/gdb" \
       "$ROOTFS_DIR/usr/share/bug" "$ROOTFS_DIR/usr/share/bash-completion"
# Keep only en and zh locales (strip the other ~100MB)
if [ -d "$ROOTFS_DIR/usr/share/locale" ]; then
    find "$ROOTFS_DIR/usr/share/locale" -maxdepth 1 -type d \
         ! -name 'locale' ! -name 'en*' ! -name 'zh*' ! -name 'C*' \
         -exec rm -rf {} + 2>/dev/null || true
fi
# Remove static libraries (not needed at runtime; save ~10M)
find "$ROOTFS_DIR/usr/lib" -name '*.a' -delete 2>/dev/null || true
# Remove pacman package cache
rm -rf "$ROOTFS_DIR/var/cache/pacman/pkg"/* 2>/dev/null || true
echo "[arch-rootfs]   after:  $(du -sh "$ROOTFS_DIR" | cut -f1)"

# 5.6 Redirect writable paths into tmpfs (rootfs itself mounts read-only —
#     the UTSM virtio-blk rootfs backend is a read-only memory image).
#     /tmp is a tmpfs mounted by /sbin/init, so symlinking these paths
#     makes pacman -Sy and DNS resolution work in the guest:
#       /var/lib/pacman   → pacman sync databases (pacman -Sy writes here)
#       /var/cache/pacman → downloaded package cache
#       /etc/resolv.conf  → DNS config written by /sbin/init
echo "[arch-rootfs] Redirecting writable paths to /tmp (tmpfs)..."
# pacman 的 local 数据库（var/lib/pacman/local）记录“系统里装了哪些包”，
# 是 pacman -S 判断文件冲突的唯一依据。原脚本把它和 var/lib/pacman 一起
# 换成指向 /tmp 的符号链接，等于把数据库整个丢掉 —— 结果是 pacman 认为
# 系统是空的，安装 base/core 系依赖时报 "exists in filesystem" 而必定失败。
#
# 做法：把 pristine local 库留在只读镜像里（/usr/local/share/deshab），
# 启动时由 guest 启动脚本复制进 tmpfs 再给 pacman 用。镜像侧仍只读、
# 不产生 virtio 脏页；pacman 侧拿到一份能写的、内容完整的库。
PRISTINE_DB="$ROOTFS_DIR/usr/local/share/deshab/pacman-local"
if [ -d "$ROOTFS_DIR/var/lib/pacman/local" ]; then
    mkdir -p "$(dirname "$PRISTINE_DB")"
    rm -rf "$PRISTINE_DB"
    cp -a "$ROOTFS_DIR/var/lib/pacman/local" "$PRISTINE_DB"
    echo "[arch-rootfs]   pristine pacman local db: $(find "$PRISTINE_DB" -maxdepth 1 -mindepth 1 -type d | wc -l) pkgs"
else
    echo "[arch-rootfs]   WARNING: bootstrap has no pacman local db"
fi

rm -rf "$ROOTFS_DIR/var/lib/pacman" "$ROOTFS_DIR/var/cache/pacman"
ln -sfn /tmp/pacman/lib   "$ROOTFS_DIR/var/lib/pacman"
ln -sfn /tmp/pacman/cache "$ROOTFS_DIR/var/cache/pacman"

# 构建期还要 chroot 进 rootfs 跑 pacman -S（python / wifi 工具）。那些调用
# 走的是 /var/lib/pacman → /tmp/pacman/lib，所以这里先把母本种进
# ROOTFS_DIR/tmp/pacman/lib/local，构建期 pacman 才有已装包清单可用。
# 这份副本只是构建期脚手架：运行期 /tmp 是 tmpfs，启动 wrapper 会从
# /usr/local/share/deshab/pacman-local 重新种一份，镜像里的副本不可见。
# （打包前在 7.8 清掉，避免白白多占 4MB 镜像空间。）
mkdir -p "$ROOTFS_DIR/tmp/pacman/lib/sync" "$ROOTFS_DIR/tmp/pacman/cache/pkg"
if [ -d "$PRISTINE_DB" ]; then
    rm -rf "$ROOTFS_DIR/tmp/pacman/lib/local"
    cp -a "$PRISTINE_DB" "$ROOTFS_DIR/tmp/pacman/lib/local"
fi
rm -f "$ROOTFS_DIR/etc/resolv.conf"
ln -sfn /tmp/resolv.conf  "$ROOTFS_DIR/etc/resolv.conf"

# 6. Create /sbin/init — runs AFTER switch_root from initramfs.
#    Starts utsm_exec_daemon (UTSM Linux compat layer), then drops to shell.
cat > "$ROOTFS_DIR/sbin/init" <<'INITSCRIPT'
#!/bin/bash
# Arch rootfs init - runs after switch_root from initramfs
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t devtmpfs none /dev 2>/dev/null
mount -t tmpfs none /tmp 2>/dev/null
mount -t tmpfs none /run 2>/dev/null

# Writable dirs for pacman on the read-only rootfs (see build script 5.6:
# /var/lib/pacman and /var/cache/pacman are symlinks into this tmpfs).
mkdir -p /tmp/pacman/lib/sync /tmp/pacman/cache/pkg

echo "arch-utsm" > /etc/hostname

# Configure network (virtio-net should be eth0)
# UTSM host bridges virtio-net → QEMU e1000 → slirp user networking,
# whose subnet is fixed at 10.0.2.0/24 (gw 10.0.2.2, DNS 10.0.2.3).
# Use a static config so pacman works without a DHCP client in rootfs.
ip link set lo up 2>/dev/null
if ip link set eth0 up 2>/dev/null; then
    ip addr add 10.0.2.15/24 dev eth0 2>/dev/null
    ip route add default via 10.0.2.2 dev eth0 2>/dev/null
    echo "nameserver 10.0.2.3" > /etc/resolv.conf
    echo "[arch-utsm] eth0: 10.0.2.15/24 gw 10.0.2.2 (slirp static)"
else
    echo "[arch-utsm] WARNING: eth0 not found — network unavailable"
fi

# ===== Deshab guest WiFi (mac80211_hwsim virtual radios) =====
# The guest kernel builds mac80211_hwsim in, so two radios (wlan0 station /
# wlan1 AP) appear at boot. hostapd runs a demo AP on wlan1
# (SSID "Deshab-Net" / WPA2 / password "deshab123") so the Deshab desktop
# network icon can scan a real SSID, take a password, and perform a genuine
# 802.11 association + WPA2 4-way handshake. Actual internet backhaul stays
# on eth0 (slirp) above.
rfkill unblock all 2>/dev/null
ip link set wlan0 up 2>/dev/null
if command -v iw >/dev/null 2>&1 && iw dev 2>/dev/null | grep -q wlan1; then
    cat > /tmp/hostapd-deshab.conf <<'HAPD'
interface=wlan1
driver=nl80211
ssid=Deshab-Net
hw_mode=g
channel=1
wpa=2
wpa_passphrase=deshab123
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
HAPD
    iw dev wlan1 set type __ap 2>/dev/null
    ip link set wlan1 up 2>/dev/null
    if command -v hostapd >/dev/null 2>&1 && \
       hostapd -B /tmp/hostapd-deshab.conf >/tmp/hostapd.log 2>&1; then
        echo "[arch-utsm] hostapd demo AP up (SSID Deshab-Net)"
    else
        echo "[arch-utsm] WARNING: hostapd failed to start (see /tmp/hostapd.log)"
    fi
else
    echo "[arch-utsm] WARNING: no hwsim wlan1 / iw missing — WiFi demo disabled"
fi

echo "[arch-utsm] Arch Linux rootfs booted"
echo "[arch-utsm] Kernel: $(uname -r)"
echo "[arch-utsm] Architecture: $(uname -m)"

# ===== VSCode Phase 4: persistent rw volume (/dev/vdc) =====
# The extra-rootfs image is exposed as a WRITABLE memory-backed virtio-blk
# device (slot6, GPA 0xF4006000, IRQ11) by the UTSM extra_rootfs backend.
# We mount it at /mnt/persist and bind-mount its subtrees over the volatile
# paths VSCode and user data live in, so they survive tmpfs cleanup and
# (Phase 7) can be written back to the FAT32 ESP for true persistence.
if [ -b /dev/vdc ]; then
    mkdir -p /mnt/persist
    if mount -t ext4 -o rw /dev/vdc /mnt/persist 2>/dev/null; then
        echo "[arch-utsm] /dev/vdc mounted rw at /mnt/persist (extra-rootfs)"
        # Persistent subtrees: /opt (VSCode install), /home (user data),
        # /root (daemon cwd), /var/lib/pacman is left on tmpfs (rebuildable).
        for d in opt home root; do
            mkdir -p "/mnt/persist/$d"
            mount --bind "/mnt/persist/$d" "/$d" 2>/dev/null && \
                echo "[arch-utsm] bind: /$d → persist" || true
        done
    else
        echo "[arch-utsm] WARNING: /dev/vdc mount failed (not ext4?) — rw layer disabled"
    fi
else
    echo "[arch-utsm] no /dev/vdc — running fully volatile (tmpfs only)"
fi

# Check if pacman is available
if command -v pacman &>/dev/null; then
    echo "[arch-utsm] pacman available: $(pacman --version | head -1)"
else
    echo "[arch-utsm] WARNING: pacman not found"
fi

# Check if running under UTSM
if [ -e /dev/utsm ]; then
    echo "[arch-utsm] UTSM device detected, compat layer active"
fi

# Start exec daemon (UTSM Linux compat layer).
# The daemon sends EXEC_READY then ioctl(PARK)→HLT, which parks the guest
# and lets linux_launch() return to UTSM. On `linux <prog>` from Deshab
# shell, UTSM vmresume's the guest and the daemon executes the program.
if [ -x /usr/local/bin/utsm_exec_daemon ]; then
    echo "[arch-utsm] Starting UTSM exec daemon..."
    /usr/local/bin/utsm_exec_daemon &
    DAEMON_PID=$!
    echo "[arch-utsm] exec daemon PID=$DAEMON_PID"
else
    echo "[arch-utsm] WARNING: utsm_exec_daemon not found — compat layer disabled"
fi

# Interactive shell on console (keeps PID 1 alive).
# If the daemon is running, this shell coexists; the daemon parks via HLT
# and UTSM resumes it on demand. This shell is for direct guest debugging.
echo "[arch-utsm] Starting shell on console"
exec /bin/bash -l </dev/console >/dev/console 2>&1
INITSCRIPT
chmod +x "$ROOTFS_DIR/sbin/init"

# 7. Create /etc/fstab
cat > "$ROOTFS_DIR/etc/fstab" <<'FSTAB'
# /etc/fstab - Arch UTSM guest
proc /proc proc defaults 0 0
sysfs /sys sysfs defaults 0 0
devtmpfs /dev devtmpfs defaults 0 0
tmpfs /tmp tmpfs defaults 0 0
tmpfs /run tmpfs defaults 0 0
FSTAB

# 7.5 Build & install utsm_exec_daemon into the Arch rootfs
#     Static binary — runs as PID 2 after Arch /sbin/init. Parks the guest
#     via ioctl(PARK)→HLT and handles UTSM exec requests on vmresume.
DAEMON_SRC="$SCRIPT_DIR/initramfs/bin/utsm_exec_daemon.c"
IPC_INCLUDE="$PROJECT_ROOT/CODE/utsm-ipc"
if [ -f "$DAEMON_SRC" ]; then
    echo "[arch-rootfs] Compiling utsm_exec_daemon (static)..."
    mkdir -p "$ROOTFS_DIR/usr/local/bin"
    # 最小 nl80211 AP 工具（零 OpenSSL；hostapd 因 guest libcrypto 缺陷不可用）
    AP_SRC="$SCRIPT_DIR/deshab_ap.c"
    if [ -f "$AP_SRC" ]; then
        if cc -static -O2 -Wall -Wno-unused-function -o \
               "$ROOTFS_DIR/usr/local/bin/deshab_ap" "$AP_SRC" 2>"$WORK_DIR/ap_build.log"; then
            strip "$ROOTFS_DIR/usr/local/bin/deshab_ap" 2>/dev/null || true
            chmod +x "$ROOTFS_DIR/usr/local/bin/deshab_ap"
            echo "[arch-rootfs] deshab_ap installed"
        else
            echo "[arch-rootfs] WARNING: deshab_ap build failed:"; cat "$WORK_DIR/ap_build.log"
        fi
    fi
    if cc -static -O2 -Wall "-I$IPC_INCLUDE" \
           -o "$ROOTFS_DIR/usr/local/bin/utsm_exec_daemon" "$DAEMON_SRC" 2>"$WORK_DIR/daemon_build.log"; then
        strip "$ROOTFS_DIR/usr/local/bin/utsm_exec_daemon" 2>/dev/null || true
        chmod +x "$ROOTFS_DIR/usr/local/bin/utsm_exec_daemon"
        # initramfs /init (CODE/linux/initramfs/init) switch_roots STRAIGHT to
        # this entry — /sbin/init never runs — so guest networking (eth0 uplink
        # + mac80211_hwsim demo AP for the desktop WiFi card) must be brought up
        # here, before the vCPU parks for UTSM. Replace the entry with a wrapper
        # that does the setup then execs the real daemon (keeping it PID 1).
        mv -f "$ROOTFS_DIR/usr/local/bin/utsm_exec_daemon" \
              "$ROOTFS_DIR/usr/local/bin/utsm_exec_daemon.real"
        # 启动 wrapper 用仓库里的 guest_boot.sh（与 patch_rootfs_pacman.sh 共用同一份，
        # 避免逻辑两处漂移）。它铺 tmpfs、pacman 可写层、网络与演示 AP，最后 exec 真 daemon。
        GUEST_BOOT="$SCRIPT_DIR/initramfs/guest_boot.sh"
        if [ ! -f "$GUEST_BOOT" ]; then
            echo "[arch-rootfs] ERROR: missing $GUEST_BOOT" >&2
            exit 1
        fi
        sed 's|%DAEMON%|/usr/local/bin/utsm_exec_daemon.real|g' \
            "$GUEST_BOOT" > "$ROOTFS_DIR/usr/local/bin/utsm_exec_daemon"
        chmod +x "$ROOTFS_DIR/usr/local/bin/utsm_exec_daemon"
        echo "[arch-rootfs] daemon + boot-wrapper installed (from guest_boot.sh)"
    else
        echo "[arch-rootfs] WARNING: daemon build failed:"
        cat "$WORK_DIR/daemon_build.log"
    fi
else
    echo "[arch-rootfs] WARNING: daemon source not found at $DAEMON_SRC"
fi

# 7.6 Distro CPython for guest userspace (Track C). Keep interpreter and
# stdlib from the same Arch packages; do not overlay SYSTEM/ESP/lib libpython.
if [ "$ARCH_INSTALL_PYTHON" = "1" ]; then
    echo "[arch-rootfs] Installing Arch CPython into staging rootfs..."
    mkdir -p "$ROOTFS_DIR"/{dev,proc,sys,tmp}
    mount --bind /dev "$ROOTFS_DIR/dev" 2>/dev/null || true
    mount --bind /proc "$ROOTFS_DIR/proc" 2>/dev/null || true
    mount --bind /sys "$ROOTFS_DIR/sys" 2>/dev/null || true
    printf 'nameserver 1.1.1.1\n' > "$ROOTFS_DIR/tmp/resolv.conf"
    # 5.6 把 /var/lib/pacman 指到 /tmp/pacman/lib（tmpfs）；构建期需先建好目标目录，
    # 否则 chroot 内 pacman 报 "could not find or read directory"。
    mkdir -p "$ROOTFS_DIR/tmp/pacman/lib/sync" "$ROOTFS_DIR/tmp/pacman/cache/pkg"
    if chroot "$ROOTFS_DIR" /usr/bin/pacman -Sy --noconfirm && \
       chroot "$ROOTFS_DIR" /usr/bin/pacman -S --noconfirm --overwrite '*' python; then
        mkdir -p "$ROOTFS_DIR/usr/local/share/deshab/python"
        if [ -f "$PROJECT_ROOT/SYSTEM/ESP/user/python/hello.py" ]; then
            install -m 0644 "$PROJECT_ROOT/SYSTEM/ESP/user/python/hello.py" \
                "$ROOTFS_DIR/usr/local/share/deshab/python/hello.py"
        fi
        echo "[arch-rootfs] python: $(chroot "$ROOTFS_DIR" /usr/bin/python3 --version 2>/dev/null || true)"
    else
        echo "[arch-rootfs] WARNING: python install failed (network or pacman)"
    fi
    umount "$ROOTFS_DIR/dev" 2>/dev/null || true
    umount "$ROOTFS_DIR/proc" 2>/dev/null || true
    umount "$ROOTFS_DIR/sys" 2>/dev/null || true
fi

# 7.7 Wireless userspace tools for the guest WiFi backend (mac80211_hwsim).
# iw / wpa_supplicant 驱动桌面网络图标经 guest 的扫描+关联；hostapd 起演示 AP。
if [ "${ARCH_INSTALL_WIFI:-1}" = "1" ]; then
    echo "[arch-rootfs] Installing guest WiFi tools (iw/wpa_supplicant/hostapd)..."
    mkdir -p "$ROOTFS_DIR"/{dev,proc,sys,tmp}
    mount --bind /dev "$ROOTFS_DIR/dev" 2>/dev/null || true
    mount --bind /proc "$ROOTFS_DIR/proc" 2>/dev/null || true
    mount --bind /sys "$ROOTFS_DIR/sys" 2>/dev/null || true
    printf 'nameserver 1.1.1.1\n' > "$ROOTFS_DIR/tmp/resolv.conf"
    mkdir -p "$ROOTFS_DIR/tmp/pacman/lib/sync" "$ROOTFS_DIR/tmp/pacman/cache/pkg"
    chroot "$ROOTFS_DIR" /usr/bin/pacman -Sy --noconfirm >/dev/null 2>&1 || true
    if chroot "$ROOTFS_DIR" /usr/bin/pacman -S --noconfirm --needed --overwrite '*' \
            iw wpa_supplicant hostapd; then
        echo "[arch-rootfs] wifi tools: $(chroot "$ROOTFS_DIR" /usr/bin/iw --version 2>/dev/null | head -1)"
    else
        echo "[arch-rootfs] WARNING: wifi tools install failed (network or pacman)"
    fi
    umount "$ROOTFS_DIR/dev" 2>/dev/null || true
    umount "$ROOTFS_DIR/proc" 2>/dev/null || true
    umount "$ROOTFS_DIR/sys" 2>/dev/null || true
fi

# 7.8 Post-install strip: python/wifi packages bring man/info/doc/locales back;
# drop them again so the RAM-resident boot-module image stays small.
echo "[arch-rootfs] Stripping post-install docs..."
rm -rf "$ROOTFS_DIR/usr/share/man" "$ROOTFS_DIR/usr/share/doc" \
       "$ROOTFS_DIR/usr/share/info" "$ROOTFS_DIR/usr/share/gtk-doc" \
       "$ROOTFS_DIR/usr/share/help" "$ROOTFS_DIR/usr/include" 2>/dev/null || true
if [ -d "$ROOTFS_DIR/usr/share/locale" ]; then
    find "$ROOTFS_DIR/usr/share/locale" -maxdepth 1 -type d \
         ! -name 'locale' ! -name 'en*' ! -name 'zh*' ! -name 'C*' \
         -exec rm -rf {} + 2>/dev/null || true
fi
rm -rf "$ROOTFS_DIR/var/cache/pacman/pkg"/* 2>/dev/null || true

# 7.9 pacman 母本库收尾。
# 7.6/7.7 的 chroot pacman -S 又装了 python 与 wifi 工具，/tmp/pacman/lib/local
# 是当前最完整的已装包清单。把它写回母本位置，运行期启动 wrapper 才会拿到
# 包含这些包的正确清单（否则装 python 时会误报文件冲突）。
if [ -d "$ROOTFS_DIR/tmp/pacman/lib/local" ]; then
    rm -rf "$PRISTINE_DB"
    mkdir -p "$(dirname "$PRISTINE_DB")"
    cp -a "$ROOTFS_DIR/tmp/pacman/lib/local" "$PRISTINE_DB"
    echo "[arch-rootfs]   pristine pacman local db refreshed: $(find "$PRISTINE_DB" -maxdepth 1 -mindepth 1 -type d | wc -l) pkgs"
fi
# 构建期脚手架：/tmp 运行期是 tmpfs，这份副本在 guest 里永远看不到，纯占空间。
rm -rf "$ROOTFS_DIR/tmp/pacman" 2>/dev/null || true
echo "[arch-rootfs]   staged size: $(du -sh "$ROOTFS_DIR" | cut -f1)"

# 8. Create ext4 image on native FS, populate, then copy to /mnt/d.
echo "[arch-rootfs] Creating ext4 image (${IMG_SIZE_MB}MB) on native FS..."
NATIVE_IMG="$WORK_DIR/linux-rootfs.img"
rm -f "$NATIVE_IMG"
dd if=/dev/zero of="$NATIVE_IMG" bs=1M count="$IMG_SIZE_MB" 2>/dev/null
mkfs.ext4 -F -q -L "ARCH_ROOTFS" "$NATIVE_IMG" >/dev/null

mkdir -p "$WORK_DIR/mnt"
LOOP_DEV=$(losetup -f --show "$NATIVE_IMG")
mount "$LOOP_DEV" "$WORK_DIR/mnt"

echo "[arch-rootfs] Copying rootfs into ext4 image (native FS)..."
rsync -a "$ROOTFS_DIR/" "$WORK_DIR/mnt/"

sync
umount "$WORK_DIR/mnt"
losetup -d "$LOOP_DEV"

# 9. Copy the finished image to the Windows side (single large-file write).
echo "[arch-rootfs] Copying image to $OUTPUT_IMG ..."
cp "$NATIVE_IMG" "$OUTPUT_IMG"

echo "[arch-rootfs] Image created: $OUTPUT_IMG ($(du -h "$OUTPUT_IMG" | cut -f1))"
echo "[arch-rootfs] Done. rootfs.img is a limine boot module (linux:rootfs)."
echo "[arch-rootfs] Linux guest init mounts /dev/vdb and switch_root's into it."
