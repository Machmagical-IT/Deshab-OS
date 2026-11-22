@echo off
setlocal enabledelayedexpansion
REM Native Windows QEMU: window is drawn on the Windows desktop, mouse clicks work.
REM The WSLg window opened by run_qemu.bat may not appear or accept Windows input.
REM Usage: run_qemu_win.bat [dev|release|realtest|ne]
REM Accel is TCG: WHPX crashes this guest (exit 0xC0000005 on this machine).
REM TCG has no nested VMX, so the Linux guest does not run.

set "SCRIPT_DIR=%~dp0"
set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
if not exist "%QEMU%" (
    echo [qemu] Windows QEMU not found: %QEMU%
    pause
    exit /b 1
)

set "S_DEV=missing"
if exist "%SCRIPT_DIR%deshab-dev.img" set "S_DEV=ok"
set "S_REL=missing"
if exist "%SCRIPT_DIR%deshab-release.img" set "S_REL=ok"
set "S_RT=missing"
if exist "%SCRIPT_DIR%deshab-realtest.img" set "S_RT=ok"
set "S_NE=missing"
if exist "%SCRIPT_DIR%deshab-ne.img" set "S_NE=ok"

set "VARIANT=%~1"
if not defined VARIANT (
    echo [qemu] Select image variant to boot ^(native Windows window^):
    echo   1. dev      ^(deshab-dev.img^) [%S_DEV%]
    echo   2. release  ^(deshab-release.img^) [%S_REL%]
    echo   3. realtest ^(deshab-realtest.img^) [%S_RT%]
    echo   4. ne       ^(deshab-ne.img^) [%S_NE%]
    set /p CHOICE="Choice [1-4, Enter=first existing]: "
    if "!CHOICE!"=="1" set "VARIANT=dev"
    if "!CHOICE!"=="2" set "VARIANT=release"
    if "!CHOICE!"=="3" set "VARIANT=realtest"
    if "!CHOICE!"=="4" set "VARIANT=ne"
)
REM Enter: pick the first existing image (dev -^> release -^> ne -^> realtest)
if not defined VARIANT if exist "%SCRIPT_DIR%deshab-dev.img" set "VARIANT=dev"
if not defined VARIANT if exist "%SCRIPT_DIR%deshab-release.img" set "VARIANT=release"
if not defined VARIANT if exist "%SCRIPT_DIR%deshab-ne.img" set "VARIANT=ne"
if not defined VARIANT if exist "%SCRIPT_DIR%deshab-realtest.img" set "VARIANT=realtest"
if not defined VARIANT set "VARIANT=dev"
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
    echo [qemu] Build it first: build.bat -Variant %VARIANT%
    pause
    exit /b 1
)

set "FWDIR=C:\Program Files\qemu\share"
set "CODE=%FWDIR%\edk2-x86_64-code.fd"
if not exist "%CODE%" (
    echo [qemu] UEFI firmware not found: %CODE%
    pause
    exit /b 1
)
set "BTMP=%SCRIPT_DIR%..\.build_tmp"
set "EXTRA="
if exist "%BTMP%\nvme_test.img" set "EXTRA=!EXTRA! -drive if=none,id=nvme0,file=%BTMP%\nvme_test.img,format=raw -device nvme,drive=nvme0,serial=deadbeef"
if exist "%BTMP%\usb_test.img" set "EXTRA=!EXTRA! -device qemu-xhci,id=xhci0 -drive id=usb0,if=none,file=%BTMP%\usb_test.img,format=raw -device usb-storage,bus=xhci0.0,drive=usb0"
if exist "%BTMP%\sata_fat32_dsk.img" set "EXTRA=!EXTRA! -drive id=sata0,format=raw,file=%BTMP%\sata_fat32_dsk.img,if=none -device ide-hd,drive=sata0,bus=ide.1"

REM Optional serial redirection for headless debugging: set DESHAB_QEMU_SERIAL_LOG=<file>
set "SERIAL=-serial stdio"
if defined DESHAB_QEMU_SERIAL_LOG set "SERIAL=-serial file:%DESHAB_QEMU_SERIAL_LOG%"

echo [qemu] Variant: %VARIANT%  (native Windows QEMU, TCG, GTK window)
echo [qemu] Image: %IMG%
echo [qemu] Linux guest will not run: TCG has no nested VMX. Use run_qemu.bat for KDE/VSCode.

"%QEMU%" -accel tcg -machine q35,vmport=on -cpu qemu64 -m 4G -smp 2 ^
    -display gtk,grab-on-hover=off ^
    -drive if=pflash,format=raw,readonly=on,file="%CODE%" ^
    -drive if=none,id=osdisk,format=raw,file="%IMG%" ^
    -device ide-hd,drive=osdisk,bus=ide.0,bootindex=1 ^
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 ^
    %SERIAL% %EXTRA%
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo [qemu] QEMU exited with code %RC%.
    pause
)
endlocal & exit /b %RC%
