@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"

REM ===== Variant selection: dev / release / realtest =====
REM Priority: argument > QEMU_VARIANT env > interactive menu > default dev
REM Usage: run_qemu.bat [dev|release|realtest]
set "VARIANT="
if not "%~1"=="" set "VARIANT=%~1"
if not defined VARIANT if defined QEMU_VARIANT set "VARIANT=%QEMU_VARIANT%"
if not defined VARIANT (
    echo [qemu] Select image variant to boot:
    echo   1. dev      ^(deshab-dev.img, debug all on, auto test, panic=strict^)
    echo   2. release  ^(deshab-release.img, desktop + login, panic=fallback^)
    echo   3. realtest ^(deshab-realtest.img, release options, panic=strict^)
    set /p CHOICE="Choice [1-3, Enter=dev]: "
    if "!CHOICE!"=="2" set "VARIANT=release"
    if "!CHOICE!"=="3" set "VARIANT=realtest"
    if not defined VARIANT set "VARIANT=dev"
)
REM Validate variant name (also accepts passing 1/2/3 as argument)
if "%VARIANT%"=="1" set "VARIANT=dev"
if "%VARIANT%"=="2" set "VARIANT=release"
if "%VARIANT%"=="3" set "VARIANT=realtest"
if not "%VARIANT%"=="dev" if not "%VARIANT%"=="release" if not "%VARIANT%"=="realtest" (
    echo [qemu] Unknown variant: %VARIANT% ^(expected dev / release / realtest^)
    exit /b 1
)

set "IMG=%SCRIPT_DIR%deshab-%VARIANT%.img"

if not exist "%IMG%" (
    echo [qemu] Image not found: %IMG%
    echo [qemu] Run ..\build.bat first -- default builds dev, release and realtest.
    echo [qemu] Or build selected only: build.bat -Variant %VARIANT%
    exit /b 1
)
echo [qemu] Variant: %VARIANT%
echo [qemu] Image: %IMG%
echo [qemu] Launcher: WSL KVM via run_qemu_kvm.sh

where wsl >nul 2>&1
if errorlevel 1 (
    echo [qemu] wsl was not found. run_qemu.bat boots through ISO\run_qemu_kvm.sh.
    exit /b 1
)

set "IMG_FWD=%IMG:\=/%"
set "SH_FWD=%SCRIPT_DIR:\=/%run_qemu_kvm.sh"
for /f "delims=" %%W in ('wsl wslpath -a "%IMG_FWD%"') do set "IMG_WSL=%%W"
for /f "delims=" %%W in ('wsl wslpath -a "%SH_FWD%"') do set "SH_WSL=%%W"
if not defined IMG_WSL (
    echo [qemu] wslpath failed for %IMG%
    exit /b 1
)
if not defined SH_WSL (
    echo [qemu] wslpath failed for run_qemu_kvm.sh
    exit /b 1
)

echo [qemu] WSL image: %IMG_WSL%
echo [qemu] Display: sdl
wsl bash "%SH_WSL%" %VARIANT%
set "RC=%ERRORLEVEL%"
endlocal & exit /b %RC%
