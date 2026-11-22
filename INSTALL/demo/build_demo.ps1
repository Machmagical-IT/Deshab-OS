# INSTALL/demo/build_demo.ps1
#
# 构建 DeshabOS 桌面演示 exe（单文件、无需安装）。
#
# 核心原则：启动画面与桌面 UI 直接编译原工程源码，不做重绘近似：
#   - CODE/boot/splash.c   → 引导齿轮/铆钉/40 段进度条（boot_splash.c 垫片）
#   - CODE/desktop/*.c     → Aqua 桌面外壳（-DDESHAB_HOST 宿主构建）
#
# 用法：
#   powershell -File INSTALL\demo\build_demo.ps1
#
# 依赖：msys64 mingw-w64（gcc + windres）、WSL（mkfs.vfat + mcopy）。
[CmdletBinding()]
param([switch]$SkipEsp)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
$out  = Join-Path (Split-Path -Parent $here) "DeshabOS-Demo.exe"
$obj  = Join-Path $root ".build_tmp\demo_obj"

$mingwBin = "C:\msys64\mingw64\bin"
$gcc      = Join-Path $mingwBin "gcc.exe"
$windres  = Join-Path $mingwBin "windres.exe"
if (-not (Test-Path $gcc))     { throw "DEMO-E01 未找到 gcc: $gcc" }
if (-not (Test-Path $windres)) { throw "DEMO-E02 未找到 windres: $windres" }

$mods = @(
    "main", "input", "wm", "compositor", "apps_builtin", "appreg", "launcher", "lxc",
    "shell", "pages", "f32io", "ascii_font", "zhfont", "wallpaper", "taskbar",
    "textures", "prank", "files", "launchpad", "aqua_draw", "dock", "menubar",
    "glass", "guest_banner", "motion", "blit"
)

# 1) 最小 FAT32 卷
$imgPath = Join-Path $here "esp.img"
if (-not $SkipEsp) {
    Write-Host "[demo] 生成内嵌 FAT32 卷（WSL: mkfs.vfat + mcopy）..."
    wsl -e bash -c "sed -i 's/\r$//' /mnt/d/Code/Deshab/INSTALL/demo/build_esp.sh && bash /mnt/d/Code/Deshab/INSTALL/demo/build_esp.sh" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "DEMO-E03 esp.img 生成失败" }
}
if (-not (Test-Path $imgPath)) { throw "DEMO-E04 esp.img 缺失" }

New-Item -ItemType Directory -Force -Path $obj | Out-Null
Push-Location $root
try {
    # 2) 资源
    Write-Host "[demo] windres 资源编译（含 48MB FAT32 卷）..."
    Push-Location $here
    & $windres -I. demo.rc -O coff -o (Join-Path $obj "demo_res.o")
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) { throw "DEMO-E05 windres 失败" }

    # 3) 原桌面模块（宿主构建）
    Write-Host "[demo] 编译 CODE/desktop 原模块（-DDESHAB_HOST）..."
    $common = @("-std=gnu11", "-O2", "-w", "-DDESHAB_HOST",
                "-I", "CODE/UTSM/include", "-I", "CODE/desktop", "-I", "INSTALL/demo/host")
    foreach ($m in $mods) {
        & $gcc @common -c "CODE/desktop/$m.c" -o (Join-Path $obj "$m.o")
        if ($LASTEXITCODE -ne 0) { throw "DEMO-E06 模块编译失败: $m" }
    }

    # 4) 宿主层 + 真实 splash.c 垫片
    Write-Host "[demo] 编译 Win32 宿主与引导动画..."
    & $gcc @common -Wall -Wextra -c "INSTALL/demo/host/host_main.c" -o (Join-Path $obj "host_main.o")
    if ($LASTEXITCODE -ne 0) { throw "DEMO-E07 host_main 编译失败" }
    & $gcc @common -Wall -Wextra -c "INSTALL/demo/host/boot_splash.c" -o (Join-Path $obj "boot_splash.o")
    if ($LASTEXITCODE -ne 0) { throw "DEMO-E08 boot_splash 编译失败" }

    # 5) 链接
    Write-Host "[demo] 链接..."
    $objs = Get-ChildItem $obj -Filter *.o | ForEach-Object { $_.FullName }
    $linkArgs = @("-mwindows", "-static", "-Wl,-s", "-o", $out) + $objs + @("-lgdi32", "-luser32", "-lwinmm")
    & $gcc @linkArgs
    if ($LASTEXITCODE -ne 0) { throw "DEMO-E09 链接失败" }

    $f = Get-Item $out
    Write-Host ("[demo] 完成: {0}  ({1:N1} MB)" -f $f.FullName, ($f.Length / 1MB))
} finally {
    Remove-Item $obj -Recurse -Force -ErrorAction SilentlyContinue
    Pop-Location
}
