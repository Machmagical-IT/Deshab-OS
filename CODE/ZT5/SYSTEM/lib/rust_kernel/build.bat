@echo off
setlocal enabledelayedexpansion

REM ZratroOS Rust Kernel Build Script (Windows)

set SCRIPT_DIR=%~dp0
cd /d "%SCRIPT_DIR%"

echo ========================================
echo  Building ZratroOS Rust Kernel
echo ========================================

REM 检查 Rust
where rustc >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: Rust not installed
    exit /b 1
)

for /f "delims=" %%v in ('rustc --version') do echo Rust version: %%v
for /f "delims=" %%v in ('cargo --version') do echo Cargo version: %%v
echo.

REM 添加目标
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

REM 输出
set BIN_FILE=target\x86_64-unknown-none\%BUILD_MODE%\rustknl

if exist "%BIN_FILE%" (
    echo.
    echo ========================================
    echo  Build successful!
    echo ========================================
    echo Output: %BIN_FILE%

    REM 复制
    set DEST_DIR=..\..\system\
    if not exist "!DEST_DIR!" mkdir "!DEST_DIR!"
    copy "%BIN_FILE%" "!DEST_DIR!rustknl.bin" >nul

    echo.
    echo Copied to: !DEST_DIR!rustknl.bin
    dir "!DEST_DIR!rustknl.bin" | findstr "rustknl"
) else (
    echo Error: Build failed
    exit /b 1
)
