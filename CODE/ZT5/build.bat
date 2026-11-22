@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM ZratroOS One-Click Build Script (Windows)
REM Compiles kernel + all user programs via WSL, packages FAT32 image
REM
REM Usage:
REM   build.bat                Build all + package IMG
REM   build.bat --skip-iso     Build only, skip image packaging
REM   build.bat --skip-build   Package only, skip compilation
REM   build.bat --clean        Clean all build artifacts
REM   build.bat --kernel       Build kernel only
REM   build.bat --desk         Build desktop only
REM   build.bat --help         Show help
REM ============================================================

echo.
echo ========================================
echo   ZratroOS Build System (ZT5)
echo ========================================
echo.

where wsl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] WSL not found. Install with: wsl --install
    pause
    exit /b 1
)

REM Test WSL connectivity (with retry)
set WSL_OK=0
for /L %%i in (1,1,3) do (
    if !WSL_OK! equ 0 (
        echo [INFO] Testing WSL connection (attempt %%i/3^)...
        wsl -- echo test >nul 2>&1
        if not errorlevel 1 (
            set WSL_OK=1
            echo [INFO] WSL connection OK
        ) else (
            echo [WARN] WSL not responding, retrying...
            timeout /t 2 /nobreak >nul
        )
    )
)
if !WSL_OK! equ 0 (
    echo [ERROR] WSL is not responding. Try: wsl --shutdown then retry.
    pause
    exit /b 1
)

set "SCRIPT_DIR=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build.ps1" -Args "%*"

if errorlevel 1 (
    echo.
    echo [ERROR] Build failed! Check the output above for details.
    pause
    exit /b 1
)

echo.
echo [DONE] Build completed successfully!
pause
endlocal
