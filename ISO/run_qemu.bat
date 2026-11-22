@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"

REM ===== Variant selection: dev / release / realtest / ne =====
REM Priority: argument > QEMU_VARIANT env > interactive menu > first existing image
REM Usage: run_qemu.bat [dev|release|realtest|ne]
set "VARIANT="
if not "%~1"=="" set "VARIANT=%~1"
if not defined VARIANT if defined QEMU_VARIANT set "VARIANT=%QEMU_VARIANT%"
REM Menu/default only picks an existing image so a missing dev image does not close the window
set "S_DEV=missing"
if exist "%SCRIPT_DIR%deshab-dev.img" set "S_DEV=ok"
set "S_REL=missing"
if exist "%SCRIPT_DIR%deshab-release.img" set "S_REL=ok"
set "S_RT=missing"
if exist "%SCRIPT_DIR%deshab-realtest.img" set "S_RT=ok"
set "S_NE=missing"
if exist "%SCRIPT_DIR%deshab-ne.img" set "S_NE=ok"
if not defined VARIANT (
    echo [qemu] Select image variant to boot:
    echo   1. dev      ^(deshab-dev.img, debug all on, auto test, panic=strict^) [%S_DEV%]
    echo   2. release  ^(deshab-release.img, desktop + login, panic=strict^) [%S_REL%]
    echo   3. realtest ^(deshab-realtest.img, release options, panic=strict^) [%S_RT%]
    echo   4. ne       ^(deshab-ne.img, release options, panic=fallback^) [%S_NE%]
    set /p CHOICE="Choice [1-4, Enter=first existing]: "
    if "!CHOICE!"=="1" set "VARIANT=dev"
    if "!CHOICE!"=="2" set "VARIANT=release"
    if "!CHOICE!"=="3" set "VARIANT=realtest"
    if "!CHOICE!"=="4" set "VARIANT=ne"
)
REM Enter (no choice) or missing image: pick the first existing of dev/release/ne/realtest
if not defined VARIANT if exist "%SCRIPT_DIR%deshab-dev.img" set "VARIANT=dev"
if not defined VARIANT if exist "%SCRIPT_DIR%deshab-release.img" set "VARIANT=release"
if not defined VARIANT if exist "%SCRIPT_DIR%deshab-ne.img" set "VARIANT=ne"
if not defined VARIANT if exist "%SCRIPT_DIR%deshab-realtest.img" set "VARIANT=realtest"
if not defined VARIANT set "VARIANT=dev"
REM Validate variant name (also accepts passing 1/2/3/4 as argument)
if "%VARIANT%"=="1" set "VARIANT=dev"
if "%VARIANT%"=="2" set "VARIANT=release"
if "%VARIANT%"=="3" set "VARIANT=realtest"
if "%VARIANT%"=="4" set "VARIANT=ne"
if not "%VARIANT%"=="dev" if not "%VARIANT%"=="release" if not "%VARIANT%"=="realtest" if not "%VARIANT%"=="ne" (
    echo [qemu] Unknown variant: %VARIANT% ^(expected dev / release / realtest / ne^)
    pause
    exit /b 1
)

set "IMG=%SCRIPT_DIR%deshab-%VARIANT%.img"

if not exist "%IMG%" (
    echo [qemu] Image not found: %IMG%
    echo [qemu] Run ..\build.bat first -- default builds dev and release; use -Variant all for realtest/ne.
    echo [qemu] Or build selected only: build.bat -Variant %VARIANT%
    echo [qemu] Existing images can be selected by number at the menu.
    pause
    exit /b 1
)
echo [qemu] Variant: %VARIANT%
echo [qemu] Image: %IMG%
REM NE/release/realtest use vmm=0 + linux_guest=0, so nested VMX is not needed.
REM On hosts where WSLg windows are unreliable, use the native Windows window.
if not "%VARIANT%"=="dev" if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" (
    echo [qemu] Using native Windows QEMU window ^(TCG; no nested VMX needed^).
    call "%SCRIPT_DIR%run_qemu_win.bat" %VARIANT%
    exit /b !ERRORLEVEL!
)
echo [qemu] Launcher: WSL KVM via run_qemu_kvm.sh

where wsl >nul 2>&1
if errorlevel 1 (
    echo [qemu] wsl was not found. run_qemu.bat boots through ISO\run_qemu_kvm.sh.
    pause
    exit /b 1
)

set "IMG_FWD=%IMG:\=/%"
set "SH_FWD=%SCRIPT_DIR:\=/%run_qemu_kvm.sh"
for /f "delims=" %%W in ('wsl wslpath -a "%IMG_FWD%"') do set "IMG_WSL=%%W"
for /f "delims=" %%W in ('wsl wslpath -a "%SH_FWD%"') do set "SH_WSL=%%W"
if not defined IMG_WSL (
    echo [qemu] wslpath failed for %IMG%
    pause
    exit /b 1
)
if not defined SH_WSL (
    echo [qemu] wslpath failed for run_qemu_kvm.sh
    pause
    exit /b 1
)

echo [qemu] WSL image: %IMG_WSL%
echo [qemu] Display: sdl
echo [qemu] If no window appears: run "wsl --shutdown" then retry (stale WSLg session).
REM WSLg windows can open behind the console; activate the QEMU window once it exists
start "" /b powershell -NoProfile -WindowStyle Hidden -Command "Add-Type -AssemblyName Microsoft.VisualBasic; for($i=0;$i -lt 60;$i++){ Start-Sleep -Milliseconds 500; $p=Get-Process msrdc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like '*QEMU*' } | Select-Object -First 1; if($p){ [Microsoft.VisualBasic.Interaction]::AppActivate($p.Id) | Out-Null; break } }"
wsl bash "%SH_WSL%" %VARIANT%
set "RC=%ERRORLEVEL%"
REM Double-click closes the window on exit: pause on failure so the error stays visible
if not "%RC%"=="0" (
    echo [qemu] QEMU launcher exited with code %RC%.
    pause
)
endlocal & exit /b %RC%
