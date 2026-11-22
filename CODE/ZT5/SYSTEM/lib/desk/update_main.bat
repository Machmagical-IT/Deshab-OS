@echo off
setlocal enabledelayedexpansion

REM 自动更新 main.c 中的 desk framebuffer 变量地址 (Windows 版)
REM 需要安装 GNU binutils (readelf) 并添加到 PATH

REM 路径配置 - 请根据实际情况修改
set DESK_PATH=%~dp0..\..\..\prgm\sys\desk
set MAIN_PATH=%~dp0..\..\Ztknl\main.c

REM 检查 readelf 是否可用
where readelf >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: readelf not found in PATH
    echo Please install GNU binutils or use WSL: wsl readelf
    exit /b 1
)

REM 检查文件是否存在
if not exist "%DESK_PATH%" (
    echo Error: desk binary not found at %DESK_PATH%
    exit /b 1
)
if not exist "%MAIN_PATH%" (
    echo Error: main.c not found at %MAIN_PATH%
    exit /b 1
)

REM 提取符号地址
for /f "tokens=2" %%a in ('readelf -s "%DESK_PATH%" ^| findstr " fb_base$"') do set FB_BASE=%%a
for /f "tokens=2" %%a in ('readelf -s "%DESK_PATH%" ^| findstr " fb_width$"') do set FB_WIDTH=%%a
for /f "tokens=2" %%a in ('readelf -s "%DESK_PATH%" ^| findstr " fb_height$"') do set FB_HEIGHT=%%a
for /f "tokens=2" %%a in ('readelf -s "%DESK_PATH%" ^| findstr " fb_pitch$"') do set FB_PITCH=%%a

echo Found addresses:
echo   fb_base:   %FB_BASE%
echo   fb_width:  %FB_WIDTH%
echo   fb_height: %FB_HEIGHT%
echo   fb_pitch:  %FB_PITCH%

REM 检查是否获取到地址
if "%FB_BASE%"=="" (
    echo Error: Failed to extract fb_base address
    exit /b 1
)

REM 更新 main.c (使用 PowerShell 进行正则替换)
powershell -Command "(Get-Content '%MAIN_PATH%') -replace 'uint64_t fb_base_addr = 0x[0-9a-fA-F]+;', 'uint64_t fb_base_addr = 0x%FB_BASE%;' -replace 'uint64_t fb_width_addr = 0x[0-9a-fA-F]+;', 'uint64_t fb_width_addr = 0x%FB_WIDTH%;' -replace 'uint64_t fb_height_addr = 0x[0-9a-fA-F]+;', 'uint64_t fb_height_addr = 0x%FB_HEIGHT%;' -replace 'uint64_t fb_pitch_addr = 0x[0-9a-fA-F]+;', 'uint64_t fb_pitch_addr = 0x%FB_PITCH%;' | Set-Content '%MAIN_PATH%'"

echo Updated main.c with new addresses.
