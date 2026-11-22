@echo off
setlocal enabledelayedexpansion
REM Native Windows QEMU: window is drawn on the Windows desktop, mouse clicks work.
REM The WSLg window opened by run_qemu.bat does not receive clicks from Windows.
REM Usage: run_qemu_win.bat [dev|release|realtest]
REM Accel is TCG: WHPX crashes this guest (exit 0xC0000005 on this machine).
REM TCG has no nested VMX, so the Linux guest does not run.
REM Use run_qemu.bat when KVM is needed (KDE / VSCode).

set "SCRIPT_DIR=%~dp0"
set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
if not exist "%QEMU%" (
    echo [qemu] Windows QEMU not found: %QEMU%
    exit /b 1
)

set "VARIANT=%~1"
if not defined VARIANT set "VARIANT=dev"
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
    exit /b 1
)

set "FWDIR=C:\Program Files\qemu\share"
set "CODE=%FWDIR%\edk2-x86_64-code.fd"
if not exist "%CODE%" (
    echo [qemu] UEFI firmware not found: %CODE%
    exit /b 1
)
set "BTMP=%SCRIPT_DIR%..\.build_tmp"
set "EXTRA="
if exist "%BTMP%\nvme_test.img" set "EXTRA=!EXTRA! -drive if=none,id=nvme0,file=%BTMP%\nvme_test.img,format=raw -device nvme,drive=nvme0,serial=deadbeef"
if exist "%BTMP%\usb_test.img" set "EXTRA=!EXTRA! -device qemu-xhci,id=xhci0 -drive id=usb0,if=none,file=%BTMP%\usb_test.img,format=raw -device usb-storage,bus=xhci0.0,drive=usb0"
if exist "%BTMP%\sata_fat32_dsk.img" set "EXTRA=!EXTRA! -drive id=sata0,format=raw,file=%BTMP%\sata_fat32_dsk.img,if=none -device ide-hd,drive=sata0,bus=ide.1"

echo [qemu] Variant: %VARIANT%  (native Windows QEMU, TCG, GTK window)
echo [qemu] Image: %IMG%
echo [qemu] Linux guest will not run: TCG has no nested VMX. Use run_qemu.bat for KDE/VSCode.

"%QEMU%" -accel tcg -machine q35,vmport=on -cpu qemu64 -m 4G -smp 2 ^
    -display gtk,grab-on-hover=off ^
    -drive if=pflash,format=raw,readonly=on,file="%CODE%" ^
    -drive if=none,id=osdisk,format=raw,file="%IMG%" ^
    -device ide-hd,drive=osdisk,bus=ide.0,bootindex=1 ^
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 ^
    -serial stdio %EXTRA%
endlocal & exit /b %ERRORLEVEL%
