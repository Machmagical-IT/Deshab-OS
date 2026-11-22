@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM ZratroOS NewDesk Build Script (Windows)
REM 编译新版桌面环境 (Rust) 并复制到系统程序目录
REM
REM 输出位置: SYSTEM/prgm/sys/newdesk
REM (与旧 desk 程序位于同一目录: SYSTEM/prgm/sys/)
REM
REM Usage:
REM   build.bat          Release 构建
REM   build.bat debug    Debug 构建
REM ============================================================

set SCRIPT_DIR=%~dp0
cd /d "%SCRIPT_DIR%"

echo ========================================
echo  Building ZratroOS NewDesk (Rust)
echo ========================================

REM 检查 Rust 工具链
where rustc >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: Rust not installed
    exit /b 1
)

for /f "delims=" %%v in ('rustc --version') do echo Rust version: %%v
for /f "delims=" %%v in ('cargo --version') do echo Cargo version: %%v
echo.

REM 检查并安装裸机目标
rustup target list --installed | findstr "x86_64-unknown-none" >nul 2>nul
if %errorlevel% neq 0 (
    echo Adding x86_64-unknown-none target...
    rustup target add x86_64-unknown-none
)

REM 构建模式
set BUILD_MODE=release
if "%~1"=="debug" (
    set BUILD_MODE=debug
    echo Building in DEBUG mode...
) else (
    echo Building in RELEASE mode...
)

echo.
echo Compiling...
cargo build --target x86_64-unknown-none --%BUILD_MODE% 2>&1
if %errorlevel% neq 0 (
    echo Error: Compilation failed
    exit /b 1
)

REM 检查输出文件
set BIN_FILE=target\x86_64-unknown-none\%BUILD_MODE%\newdesk.exe
if not exist "%BIN_FILE%" (
    set BIN_FILE=target\x86_64-unknown-none\%BUILD_MODE%\newdesk
)

if exist "!BIN_FILE!" (
    echo.
    echo ========================================
    echo  Build successful!
    echo ========================================
    echo Output: !BIN_FILE!

    REM 复制到 SYSTEM/prgm/sys/ 目录
    REM 与旧 desk 程序 (SYSTEM/prgm/sys/desk) 位于同一目录
    set DEST_DIR=..\..\prgm\sys
    if not exist "!DEST_DIR!" mkdir "!DEST_DIR!"
    copy "!BIN_FILE!" "!DEST_DIR!newdesk" >nul

    echo.
    echo Copied to: !DEST_DIR!\newdesk
    dir "!DEST_DIR!newdesk" | findstr "newdesk"
) else (
    echo Error: Build output not found
    exit /b 1
)
