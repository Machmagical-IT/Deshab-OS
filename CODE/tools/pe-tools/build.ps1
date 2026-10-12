# build.ps1 — 编译 Deshab PE 系统工具（mingw-w64 g++）
#
# GUI 工具 → SYSTEM/program/*.exe（开始菜单 STARTMENU.CNF 启动，窗口嵌入桌面）
# CLI 工具 → SYSTEM/bin/*.exe（CMD 里敲名字运行，输出走串口）
#
# 编译模式与 CODE/tools/pe-samples/hello64.c 相同：
#   -nostartfiles -Wl,--entry,main -Wl,--image-base,0x140000000
# （无 CRT 启动；只准调 pe_shim 表内的 Win32 面）
#
# 用法: powershell -File build.ps1 [-Gxx <g++路径>]

param([string]$GxxPath = "")

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$ProgDir = Join-Path $Root "SYSTEM\program"
$BinDir = Join-Path $Root "SYSTEM\bin"

# 定位 g++：参数 > PATH > msys2 默认位置
$gxx = $null
if ($GxxPath -and (Test-Path $GxxPath)) {
    $gxx = $GxxPath
} elseif (Get-Command g++ -ErrorAction SilentlyContinue) {
    $gxx = (Get-Command g++).Source
} elseif (Test-Path "C:\msys64\mingw64\bin\g++.exe") {
    $gxx = "C:\msys64\mingw64\bin\g++.exe"
} else {
    throw "g++ not found (install msys2 mingw-w64 or pass -GxxPath)"
}
Write-Host "[pe-tools] g++ = $gxx"

$Flags = @(
    "-O2", "-Wall", "-std=c++17",
    "-nostartfiles",
    "-Wl,--entry,main",
    "-Wl,--image-base,0x140000000"
)

# GUI 工具（user32 消息循环 + gdi32 DIB/BitBlt）
$GuiTools = @("sysinfo", "clock", "paint", "notes")
# CLI 工具（putchar/puts/ExitProcess）
$CliTools = @("pciscan", "portio")

foreach ($t in $GuiTools) {
    $src = Join-Path $PSScriptRoot "$t.cpp"
    $out = Join-Path $ProgDir "$t.exe"
    & $gxx @Flags $src -o $out -lgdi32 -luser32
    if ($LASTEXITCODE -ne 0) { throw "build failed: $t" }
    Write-Host "[pe-tools] GUI  $t -> SYSTEM\program\$t.exe"
}

foreach ($t in $CliTools) {
    $src = Join-Path $PSScriptRoot "$t.cpp"
    $out = Join-Path $BinDir "$t.exe"
    & $gxx @Flags $src -o $out
    if ($LASTEXITCODE -ne 0) { throw "build failed: $t" }
    Write-Host "[pe-tools] CLI  $t -> SYSTEM\bin\$t.exe"
}

Write-Host "[pe-tools] done. (imports must stay within pe_shim table; verify with objdump -p)"
