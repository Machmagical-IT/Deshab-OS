@echo off
REM Deshab launcher — runs the OS under WSL2 + KVM (nested VMX required for
REM the Linux guest / VSCode). The QEMU window appears on this desktop via WSLg.
REM Windows-side run_qemu.bat (WHPX) cannot start the guest: no nested VMX.
setlocal
echo [deshab] Starting WSL2 + KVM...
wsl.exe -d Ubuntu -- bash "/mnt/d/Code/DEAICUP/Deshab/ISO/launch_deshab_gui.sh"
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
    echo.
    echo [deshab] exited with code %RC%
    pause
)
endlocal
