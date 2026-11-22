# build.ps1 - Build Deshab PE system tools (mingw-w64 g++)
#
# Directory convention: bin/ = ELF tools, program/ = exe.
# All exes (GUI + CLI) -> SYSTEM\ESP\program/*.exe
#   GUI: launched from STARTMENU.CNF, windowed via run_windowed
#   CLI: run from CMD by name (cmd PATH: root -> bin/ -> program/), serial output
#
# Link model (same entry/image-base as CODE/tools/pe-samples/hello64.c, plus
# -nostdlib because g++ injects a __main call in C++ mode; pe_tool.h provides
# an empty __main - no CRT, only pe_shim-table Win32 APIs allowed).
#
# Usage: powershell -File build.ps1 [-GxxPath <path-to-g++>]
# NOTE: keep this file ASCII-only - Windows PowerShell 5.1 reads BOM-less
# files as ANSI and UTF-8 comment bytes can silently corrupt parsing.

param([string]$GxxPath = "")

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$ProgDir = Join-Path $Root "SYSTEM\ESP\program"
$BinDir = Join-Path $Root "SYSTEM\ESP\bin"

# Locate g++: arg > PATH > msys2 default
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
    "-finput-charset=UTF-8", "-fexec-charset=UTF-8",
    "-nostdlib",
    "-Wl,--entry,main",
    "-Wl,--image-base,0x140000000"
)

# GUI tools (user32 message loop + gdi32 DIB/BitBlt)
$GuiTools = @("sysinfo", "clock", "paint", "notes")
# CLI tools (putchar/puts/ExitProcess)
$CliTools = @("pciscan", "portio")

foreach ($t in $GuiTools) {
    $src = Join-Path $PSScriptRoot "$t.cpp"
    $out = Join-Path $ProgDir "$t.exe"
    & $gxx @Flags $src -o $out -lgdi32 -luser32 -lkernel32 -lmsvcrt
    if ($LASTEXITCODE -ne 0) { throw "build failed: $t" }
    Write-Host "[pe-tools] GUI  $t -> SYSTEM\ESP\program\$t.exe"
}

foreach ($t in $CliTools) {
    $src = Join-Path $PSScriptRoot "$t.cpp"
    $out = Join-Path $ProgDir "$t.exe"
    & $gxx @Flags $src -o $out -lkernel32 -lmsvcrt
    if ($LASTEXITCODE -ne 0) { throw "build failed: $t" }
    Write-Host "[pe-tools] CLI  $t -> SYSTEM\ESP\program\$t.exe"
}

Write-Host "[pe-tools] done. (imports must stay within pe_shim table; verify with objdump -p)"
