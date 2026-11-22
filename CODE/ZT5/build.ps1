param(
    [string]$Args = ""
)

$ErrorActionPreference = "Continue"

$projRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$driveLetter = $projRoot.Substring(0, 1).ToLower()
$restPath = $projRoot.Substring(2).Replace("\", "/")
$WsRoot = "/mnt/$driveLetter$restPath"

Write-Host "[INFO] Project root: $WsRoot"
Write-Host ""

$bashScript = @'
#!/bin/bash
set -euo pipefail

WSL_ROOT="__WSL_ROOT__"
SYSTEM_DIR="$WSL_ROOT/SYSTEM"
ISO_DIR="$WSL_ROOT/ISO"
ARGS="__ARGS__"
if [ -z "$ARGS" ]; then ARGS="--default"; fi

FAILED=0
BUILT_OK=()

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info()  { echo -e "${CYAN}[INFO]${NC} $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}   $*"; BUILT_OK+=("$1"); }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_err()   { echo -e "${RED}[ERROR]${NC} $*"; FAILED=$((FAILED+1)); }

build_component() {
    local name="$1"
    local dir="$2"
    local desc="$3"

    echo ""
    echo -e "${CYAN}=== Building $desc ===${NC}"
    echo "[INFO] Source: $dir"

    if [ ! -d "$dir" ]; then
        log_err "$desc: directory not found ($dir)"
        return 1
    fi

    cd "$dir"

    make clean 2>/dev/null || true

    if make -j$(nproc) 2>&1; then
        log_ok "$name"
        return 0
    else
        log_err "$desc: compilation failed"
        return 1
    fi
}

verify_file() {
    local path="$1"
    local desc="$2"
    if [ -f "$path" ]; then
        local sz
        sz=$(stat -c%s "$path" 2>/dev/null || echo '?')
        log_ok "$desc ($sz bytes)"
    else
        log_err "$desc: file not found ($path)"
    fi
}

if echo "$ARGS" | grep -q '\-\-clean'; then
    echo -e "${CYAN}=== Cleaning all build artifacts ===${NC}"
    for dir in Ztknl init shell newdesk fm wm zinstall; do
        if [ -d "$SYSTEM_DIR/lib/$dir" ]; then
            make -C "$SYSTEM_DIR/lib/$dir" clean 2>/dev/null || true
            log_info "Cleaned: $dir"
        fi
    done
    rm -rf "$ISO_DIR/iso_staging" 2>/dev/null || true
    rm -f "$ISO_DIR/zratroos.img" 2>/dev/null || true
    log_info "Clean complete"
    exit 0
fi

if echo "$ARGS" | grep -q '\-\-help'; then
    echo "ZratroOS Build System (ZT5)"
    echo ""
    echo "Usage: build.bat [option]"
    echo ""
    echo "Options:"
    echo "  (none)         Build all + package IMG"
    echo "  --skip-iso     Build only, skip image packaging"
    echo "  --skip-build   Package only, skip compilation"
    echo "  --clean        Clean all build artifacts"
    echo "  --kernel       Build kernel only"
    echo "  --desk         Build desktop only"
    echo "  --help         Show this help"
    exit 0
fi

SKIP_BUILD=0
SKIP_ISO=0
BUILD_KERNEL_ONLY=0
BUILD_DESK_ONLY=0
if echo "$ARGS" | grep -q '\-\-skip-build'; then SKIP_BUILD=1; fi
if echo "$ARGS" | grep -q '\-\-skip-iso'; then SKIP_ISO=1; fi
if echo "$ARGS" | grep -q '\-\-kernel'; then BUILD_KERNEL_ONLY=1; fi
if echo "$ARGS" | grep -q '\-\-desk'; then BUILD_DESK_ONLY=1; fi

if [ "$SKIP_BUILD" -eq 0 ]; then

    # -------------------------------------------------------
    # 1. Ztknl Kernel
    # -------------------------------------------------------
    if [ "$BUILD_DESK_ONLY" -eq 0 ]; then
        build_component "knl.bin" "$SYSTEM_DIR/lib/Ztknl" "Ztknl Kernel (x86_64 Higher Half)"
    fi

    # -------------------------------------------------------
    # 2. Init (PID 1)
    # -------------------------------------------------------
    if [ "$BUILD_KERNEL_ONLY" -eq 0 ] && [ "$BUILD_DESK_ONLY" -eq 0 ]; then
        build_component "init" "$SYSTEM_DIR/lib/init" "Init Process (PID 1)"
    fi

    # -------------------------------------------------------
    # 3. Window Manager (zrdemage)
    # -------------------------------------------------------
    if [ "$BUILD_KERNEL_ONLY" -eq 0 ] && [ "$BUILD_DESK_ONLY" -eq 0 ]; then
        build_component "zrdemage" "$SYSTEM_DIR/lib/wm" "Window Manager (zrdemage)"
    fi

    # -------------------------------------------------------
    # 4. Desktop Environment (newdesk - Rust)
    # -------------------------------------------------------
    if [ "$BUILD_KERNEL_ONLY" -eq 0 ]; then
        echo ""
        echo -e "${CYAN}=== Building Desktop Environment (newdesk/Rust) ===${NC}"
        echo "[INFO] Source: $SYSTEM_DIR/lib/newdesk"
        if [ -d "$SYSTEM_DIR/lib/newdesk" ]; then
            cd "$SYSTEM_DIR/lib/newdesk"
            # 尝试多种方式找到 cargo (rustup 安装 / apt 安装)
            export PATH="$HOME/.cargo/bin:$HOME/.rustup/toolchains/*/bin:/usr/bin:$PATH"
            if command -v cargo >/dev/null 2>&1; then
                # Ensure x86_64-unknown-none target is installed
                if command -v rustup >/dev/null 2>&1; then
                    rustup target list --installed 2>/dev/null | grep -q "x86_64-unknown-none" || rustup target add x86_64-unknown-none 2>/dev/null || true
                fi
                if cargo build --target x86_64-unknown-none --release 2>&1; then
                    # 复制到 SYSTEM/prgm/sys/desk (与旧 desk 相同位置)
                    mkdir -p "$SYSTEM_DIR/prgm/sys"
                    cp -f target/x86_64-unknown-none/release/newdesk "$SYSTEM_DIR/prgm/sys/desk"
                    log_ok "newdesk"
                else
                    log_err "newdesk: cargo build failed"
                fi
            else
                log_err "newdesk: cargo not found (install via: sudo apt install rustup && rustup default stable)"
            fi
        else
            log_err "newdesk: directory not found"
        fi
    fi

    # -------------------------------------------------------
    # 5. File Manager
    # -------------------------------------------------------
    if [ "$BUILD_KERNEL_ONLY" -eq 0 ] && [ "$BUILD_DESK_ONLY" -eq 0 ]; then
        build_component "fm" "$SYSTEM_DIR/lib/fm" "File Manager"
    fi

    # -------------------------------------------------------
    # 6. Shell
    # -------------------------------------------------------
    if [ "$BUILD_KERNEL_ONLY" -eq 0 ] && [ "$BUILD_DESK_ONLY" -eq 0 ]; then
        echo ""
        echo -e "${CYAN}=== Building Shell ===${NC}"
        echo "[INFO] Source: $SYSTEM_DIR/lib/shell"
        if [ -d "$SYSTEM_DIR/lib/shell" ]; then
            cd "$SYSTEM_DIR/lib/shell"
            make clean 2>/dev/null || true
            if make -j$(nproc) 2>&1; then
                if [ -f "$SYSTEM_DIR/prgm/shell" ]; then
                    log_ok "shell"
                else
                    log_warn "shell: built but output not at expected path"
                fi
            else
                log_err "Shell: compilation failed"
            fi
        else
            log_err "Shell: directory not found"
        fi
    fi

    # -------------------------------------------------------
    # 7. Zinstall (CLI + GUI)
    # -------------------------------------------------------
    if [ "$BUILD_KERNEL_ONLY" -eq 0 ] && [ "$BUILD_DESK_ONLY" -eq 0 ]; then
        build_component "zinstall" "$SYSTEM_DIR/lib/zinstall" "Software Installer (CLI+GUI)"
    fi

    # -------------------------------------------------------
    # 8. Rsknl (Rust Monitor Kernel)
    # -------------------------------------------------------
    if [ "$BUILD_KERNEL_ONLY" -eq 0 ] && [ "$BUILD_DESK_ONLY" -eq 0 ]; then
        echo ""
        echo -e "${CYAN}=== Building Rsknl (Rust Monitor Kernel) ===${NC}"
        cd "$SYSTEM_DIR/lib/rust_kernel"
        export PATH="$HOME/.cargo/bin:$HOME/.rustup/toolchains/*/bin:$PATH"
        if command -v cargo >/dev/null 2>&1; then
            if cargo build --lib --release 2>&1; then
                mkdir -p "$SYSTEM_DIR/zrr64/rstk"
                cp -f target/x86_64-unknown-none/release/librustknl.a "$SYSTEM_DIR/zrr64/rstk/rustknl" 2>/dev/null || \
                cp -f target/x86_64-unknown-none/release/libzratros_kernel.a "$SYSTEM_DIR/zrr64/rstk/rustknl" 2>/dev/null || true
                log_ok "rustknl"
            else
                log_warn "Rsknl: cargo build failed (non-fatal)"
            fi
        else
            log_warn "Rsknl: SKIPPED (cargo not found)"
        fi
    fi

    # -------------------------------------------------------
    # Verify all build outputs
    # -------------------------------------------------------
    echo ""
    echo -e "${CYAN}=== Verifying Build Outputs ===${NC}"

    verify_file "$SYSTEM_DIR/system/knl.bin"     "Kernel (knl.bin)"
    verify_file "$SYSTEM_DIR/prgm/sys/init"      "Init (PID 1)"
    verify_file "$SYSTEM_DIR/prgm/sys/zrdemage"  "Window Manager"
    verify_file "$SYSTEM_DIR/prgm/sys/desk"      "Desktop"
    verify_file "$SYSTEM_DIR/prgm/windowprogram/fm"       "File Manager"
    verify_file "$SYSTEM_DIR/prgm/sys/zinstall"           "Installer CLI"
    verify_file "$SYSTEM_DIR/prgm/windowprogram/zinstall" "Installer GUI"
    verify_file "$SYSTEM_DIR/prgm/shell"         "Shell"
    verify_file "$SYSTEM_DIR/zrr64/rstk/rustknl" "Rsknl"
    verify_file "$SYSTEM_DIR/EFI/BOOT/BOOTX64.EFI" "UEFI Bootloader"
    verify_file "$SYSTEM_DIR/boot/limine.conf"   "Limine Config"
fi

# -------------------------------------------------------
# Build FAT32 UEFI Bootable Image
# -------------------------------------------------------
if [ "$SKIP_ISO" -eq 0 ]; then
    echo ""
    echo -e "${CYAN}=== Building FAT32 UEFI Image ===${NC}"

    ISO_STAGING="$ISO_DIR/iso_staging"
    IMG_OUTPUT="$ISO_DIR/zratroos.img"

    rm -rf "$ISO_STAGING"
    mkdir -p "$ISO_STAGING"

    echo "[INFO] Copying SYSTEM files to staging..."
    for item in "$SYSTEM_DIR"/*/; do
        n=$(basename "$item")
        case "$n" in
            lib)
                echo "[INFO]   [skip] lib/ (source only)"
                ;;
            *)
                echo "[INFO]   [copy] $n/"
                cp -a "$item" "$ISO_STAGING/$n" 2>/dev/null || log_warn "Failed to copy $n/"
                ;;
        esac
    done

    printf '@echo -off\nFS0:\\EFI\\BOOT\\BOOTX64.EFI\n' > "$ISO_STAGING/startup.nsh"
    echo "[INFO]   [create] startup.nsh (UEFI auto-boot)"

    STAGING_SIZE=$(du -sm "$ISO_STAGING" 2>/dev/null | cut -f1)
    IMAGE_MB=$((STAGING_SIZE + 32))
    echo "[INFO] Staging: ${STAGING_SIZE}MB -> Image: ${IMAGE_MB}MB"

    echo "[INFO] Creating raw disk image (${IMAGE_MB}MB)..."
    dd if=/dev/zero of="$IMG_OUTPUT" bs=1M count=$IMAGE_MB status=progress 2>&1 | tail -1

    echo "[INFO] Creating GPT partition table (EFI System Partition)..."
    sgdisk --clear \
        --new=1:2048:0 \
        --typecode=1:C12A7328-F81F-11D2-BA4B-00A0C93EC93B \
        --attributes=1:set:2 \
        "$IMG_OUTPUT" 2>&1 | tail -3

    PART_OFFSET=$((2048 * 512))

    echo "[INFO] Formatting FAT32 partition and copying files..."
    export IMG_PATH="$IMG_OUTPUT" PART_OFFSET="$PART_OFFSET" STAGING="$ISO_STAGING"

    python3 << 'PYEOF'
import subprocess, os, sys

IP = os.environ["IMG_PATH"]
PO = int(os.environ["PART_OFFSET"])
ST = os.environ["STAGING"]

part_size = os.path.getsize(IP) - PO
tmp_part = IP + ".part"

with open(IP, "rb") as f:
    f.seek(PO)
    part_data = f.read(part_size)

with open(tmp_part, "wb") as f:
    f.write(part_data)

r = subprocess.run(
    ["mkfs.vfat", "-F", "32", "-n", "ZRATROOS", tmp_part],
    capture_output=True, text=True
)
if r.returncode != 0:
    print("mkfs.vfat failed: " + r.stderr, file=sys.stderr)
    os.unlink(tmp_part)
    sys.exit(1)
print("[INFO] FAT32 partition formatted")

from pyfatfs.PyFatFS import PyFatFS
fs = PyFatFS(filename=tmp_part)

file_count = 0
for root, dirs, files in os.walk(ST):
    rel = os.path.relpath(root, ST)
    if rel == "lib" or rel.startswith("lib" + os.sep):
        continue
    fat_path = "/" if rel == "." else "/" + rel.replace(os.sep, "/")
    try:
        fs.makedirs(fat_path)
    except Exception:
        pass
    for fn in files:
        src = os.path.join(root, fn)
        dst = fat_path + "/" + fn if fat_path != "/" else "/" + fn
        try:
            with open(src, "rb") as rf:
                data = rf.read()
            with fs.open(dst, "wb") as wf:
                wf.write(data)
            file_count += 1
        except Exception as e:
            print(f"[WARN] Failed to copy {dst}: {e}")

fs.close()
print(f"[INFO] Copied {file_count} files to FAT32 partition")

with open(tmp_part, "rb") as f:
    new_part_data = f.read()
with open(IP, "r+b") as f:
    f.seek(PO)
    f.write(new_part_data)
os.unlink(tmp_part)
print("[INFO] UEFI bootable image created successfully")
PYEOF

    if [ $? -ne 0 ]; then
        log_err "FAT32 image creation failed"
        echo "[HINT] Required tools: sgdisk (gdisk), mkfs.vfat (dosfstools), python3 with pyfatfs"
        echo "[HINT] Install: sudo apt install gdisk dosfstools python3-pip && pip3 install pyfatfs"
    else
        rm -rf "$ISO_STAGING" 2>/dev/null || true
        IMG_SIZE=$(du -sh "$IMG_OUTPUT" 2>/dev/null | cut -f1)
        log_ok "zratroos.img ($IMG_SIZE)"
    fi
fi

# -------------------------------------------------------
# Summary
# -------------------------------------------------------
echo ""
echo "========================================"
if [ "$FAILED" -eq 0 ]; then
    echo -e "  ${GREEN}ZratroOS Build Complete!${NC}"
else
    echo -e "  ${RED}ZratroOS Build: $FAILED component(s) FAILED${NC}"
fi
echo "========================================"
echo ""
echo "Build outputs:"
echo "  Kernel:       SYSTEM/system/knl.bin"
echo "  Init:         SYSTEM/prgm/sys/init"
echo "  Window Mgr:   SYSTEM/prgm/sys/zrdemage"
echo "  Desktop:      SYSTEM/prgm/sys/desk"
echo "  File Manager: SYSTEM/prgm/windowprogram/fm"
echo "  Installer:    SYSTEM/prgm/sys/zinstall (CLI)"
echo "  Software Ctr: SYSTEM/prgm/windowprogram/zinstall (GUI)"
echo "  Shell:        SYSTEM/prgm/shell"
echo "  Rsknl:        SYSTEM/zrr64/rstk/rustknl"
echo "  Boot image:   ISO/zratroos.img"
echo ""

if [ "$FAILED" -gt 0 ]; then exit 1; fi
'@

$bashScript = $bashScript -replace '__WSL_ROOT__', $WsRoot
$bashScript = $bashScript -replace '__ARGS__', ($Args -replace "'", "\'")

$winTmpPath = Join-Path $env:TEMP "zratro_build.sh"
$driveLtr = $winTmpPath.Substring(0, 1).ToLower()
$restP = $winTmpPath.Substring(2).Replace("\", "/")
$wslScriptPath = "/mnt/$driveLtr$restP"

Write-Host "[INFO] Writing build script to: $winTmpPath"
[System.IO.File]::WriteAllText($winTmpPath, $bashScript, (New-Object System.Text.UTF8Encoding($false)))

Write-Host "[INFO] Executing build via WSL..."
Write-Host ""

& wsl bash $wslScriptPath
$exitCode = $LASTEXITCODE

Remove-Item $winTmpPath -ErrorAction SilentlyContinue

if ($exitCode -ne 0) {
    Write-Host ""
    Write-Host "[ERROR] Build exited with code $exitCode" -ForegroundColor Red
    Write-Host "[HINT] If WSL failed, try: wsl --shutdown, then re-run" -ForegroundColor Yellow
}

exit $exitCode
