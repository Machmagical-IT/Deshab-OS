@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM ZTS Module Build Toolchain (Windows)
REM
REM Compiles Linux kernel source code into .zts modules for ZratroOS.
REM Uses the ZTS adaptation layer to map Linux APIs to ZTS APIs.
REM
REM Usage:
REM   build_zts.bat [command] [options]
REM
REM Commands:
REM   init                    Initialize ZTS project structure
REM   build ^<module^>          Build a specific module
REM   build_all               Build all modules in modules/
REM   list                    List available modules
REM   clean                   Clean build artifacts
REM ============================================================

REM Configuration
set ZTS_ROOT=%~dp0
set ZTS_OUT=%ZTS_ROOT%zts_modules
set ZTS_SRC=%ZTS_ROOT%modules
set ZTS_ADAPT=%ZTS_ROOT%zts_adapt.h
if not defined ZTS_CC set ZTS_CC=gcc
set ZTS_CFLAGS=-nostdlib -fPIC -shared -O2 -Wall -fno-stack-protector -fno-builtin
set ZTS_LDFLAGS=-shared
if not defined LINUX_SRC set LINUX_SRC=%ZTS_ROOT%linux

REM Main dispatch
if "%~1"=="" goto :usage
if "%~1"=="init" goto :init_project
if "%~1"=="build" goto :build_module
if "%~1"=="build_all" goto :build_all
if "%~1"=="build-all" goto :build_all
if "%~1"=="all" goto :build_all
if "%~1"=="list" goto :list_modules
if "%~1"=="ls" goto :list_modules
if "%~1"=="clean" goto :clean
if "%~1"=="-h" goto :usage
if "%~1"=="--help" goto :usage
if "%~1"=="help" goto :usage

echo [ERROR] Unknown command: %~1
goto :usage

REM ============================================================
REM Initialize Project Structure
REM ============================================================
:init_project
echo [INFO] Initializing ZTS project structure...

if not exist "%ZTS_OUT%" mkdir "%ZTS_OUT%"
if not exist "%ZTS_SRC%" mkdir "%ZTS_SRC%"
if not exist "%ZTS_SRC%\network" mkdir "%ZTS_SRC%\network"
if not exist "%ZTS_SRC%\filesystem" mkdir "%ZTS_SRC%\filesystem"
if not exist "%ZTS_SRC%\driver" mkdir "%ZTS_SRC%\driver"
if not exist "%ZTS_SRC%\scheduler" mkdir "%ZTS_SRC%\scheduler"
if not exist "%ZTS_SRC%\memory" mkdir "%ZTS_SRC%\memory"
if not exist "%ZTS_SRC%\crypto" mkdir "%ZTS_SRC%\crypto"
if not exist "%ZTS_ROOT%include" mkdir "%ZTS_ROOT%include"

REM Copy adaptation header
if exist "%ZTS_ADAPT%" (
    copy "%ZTS_ADAPT%" "%ZTS_ROOT%include\zts_adapt.h" >nul
    echo [SUCCESS] ZTS adaptation header installed
) else (
    echo [WARN] zts_adapt.h not found, please copy it manually
)

REM Check Linux kernel source
if not exist "%LINUX_SRC%" (
    echo [INFO] Linux kernel source not found at %LINUX_SRC%
    echo [INFO] To clone Linux kernel:
    echo [INFO]   git clone --depth 1 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git %LINUX_SRC%
)

REM Create example module
call :create_example_module

echo [SUCCESS] ZTS project initialized
echo.
echo Project structure:
echo   %ZTS_ROOT%
echo   ├── include\          # ZTS headers
echo   ├── modules\          # Module source code
echo   │   ├── network\      # Network modules
echo   │   ├── filesystem\   # Filesystem modules
echo   │   ├── driver\       # Driver modules
echo   │   └── ...
echo   ├── zts_modules\      # Compiled .zts modules
echo   └── build_zts.bat     # This script
goto :eof

REM ============================================================
REM Create Example Module
REM ============================================================
:create_example_module
set example_dir=%ZTS_SRC%\network\tcp_cubic
if not exist "%example_dir%" mkdir "%example_dir%"

(
echo /*
echo  * TCP CUBIC Congestion Control - ZTS Module
echo  *
echo  * Adapted from Linux kernel net/ipv4/tcp_cubic.c
echo  * Replaces ZratroOS TCP Reno with CUBIC algorithm.
echo  */
echo.
echo #include "zts_adapt.h"
echo.
echo /* CUBIC parameters */
echo #define BICTCP_BETA_SCALE    1024
echo #define BICTCP_HZ            10
echo #define BICTCP_MIN_BETA      614     /* 0.6 */
echo #define BICTCP_MAX_BETA      819     /* 0.8 */
echo.
echo /* CUBIC state */
echo struct bictcp {
echo     u32 cnt;
echo     u32 last_max_cwnd;
echo     u32 last_cwnd;
echo     u32 origin_point;
echo     u64 epoch_start;
echo     u32 delay_min;
echo     u32 ack_cnt;
echo     u32 tcp_rcv_ssthresh;
echo     u32 round_start;
echo     u32 bic_K;
echo     u32 sample_cnt;
echo };
echo.
echo static struct bictcp *cubic_data = NULL;
echo.
echo /* CUBIC initialization */
echo static int cubic_init^(void^) {
echo     cubic_data = ^(struct bictcp *^)kzalloc^(sizeof^(struct bictcp^)^);
echo     if ^(!cubic_data^) {
echo         printk^("[ZTS:CUBIC] Failed to allocate memory"^);
echo         return -ENOMEM;
echo     }
echo.
echo     printk^("[ZTS:TCP_CUBIC] TCP CUBIC congestion control loaded"^);
echo     return 0;
echo }
echo.
echo /* CUBIC congestion window calculation */
echo static int cubic_execute^(void *input, void *output, void *ctx^) {
echo     if ^(!cubic_data^) return -EINVAL;
echo.
echo     /* Process congestion control event */
echo     printk^("[ZTS:CUBIC] Congestion control executed"^);
echo     return 0;
echo }
echo.
echo /* CUBIC cleanup */
echo static void cubic_cleanup^(void^) {
echo     if ^(cubic_data^) {
echo         kfree^(cubic_data^);
echo         cubic_data = NULL;
echo     }
echo     printk^("[ZTS:TCP_CUBIC] TCP CUBIC unloaded"^);
echo }
echo.
echo /* Export as ZTS module */
echo ZTS_MODULE_INIT^(cubic_init^)
echo ZTS_MODULE_EXEC^(cubic_execute^)
echo ZTS_MODULE_CLEANUP^(cubic_cleanup^)
) > "%example_dir%\tcp_cubic.c"

echo [SUCCESS] Example TCP CUBIC module created at %example_dir%
goto :eof

REM ============================================================
REM Build All Modules
REM ============================================================
:build_all
echo [INFO] Building all ZTS modules...

if not exist "%ZTS_SRC%" (
    echo [ERROR] Source directory not found: %ZTS_SRC%
    echo [INFO] Run 'init' first to create project structure
    exit /b 1
)

set count=0
set failed=0

REM Find all directories containing .c files
for /f "delims=" %%d in ('dir /s /b "%ZTS_SRC%\*.c" 2^>nul') do (
    set src_dir=%%~dpd
    set module_name=%%~nd
    set priority=0x00000000
    set category=CUSTOM

    REM Try to read config
    if exist "!src_dir!zts.conf" (
        for /f "tokens=1,2 delims==" %%a in (!src_dir!zts.conf) do (
            if "%%a"=="priority" set priority=%%b
            if "%%a"=="category" set category=%%b
        )
    )

    call :compile_zts "!module_name!" "!src_dir!" "!priority!" "!category!"
    if !errorlevel! equ 0 (
        set /a count+=1
    ) else (
        set /a failed+=1
        echo [ERROR] Failed to build: !module_name!
    )
)

echo.
echo [SUCCESS] Build complete: %count% modules succeeded, %failed% failed
echo.
echo [INFO] Compiled modules:
if exist "%ZTS_OUT%\*.zts" (
    dir "%ZTS_OUT%\*.zts"
) else (
    echo   ^(none^)
)
goto :eof

REM ============================================================
REM Build Single Module
REM ============================================================
:build_module
if "%~2"=="" (
    echo [ERROR] Module name required
    echo Usage: %0 build ^<module_name^>
    exit /b 1
)

set module_name=%~2
set src_dir=

REM Find module source directory
for /f "delims=" %%d in ('dir /s /b /ad "%ZTS_SRC%\%module_name%" 2^>nul') do (
    set src_dir=%%d
    goto :found_module
)

REM Try finding by .c file name
for /f "delims=" %%f in ('dir /s /b "%ZTS_SRC%\%module_name%.c" 2^>nul') do (
    set src_dir=%%~dpf
    goto :found_module
)

echo [ERROR] Module '%module_name%' not found in %ZTS_SRC%
echo [INFO] Available modules:
call :list_modules
exit /b 1

:found_module
call :compile_zts "%module_name%" "%src_dir%" "0x00000000" "CUSTOM"
goto :eof

REM ============================================================
REM Compile Single Module
REM ============================================================
:compile_zts
set name=%~1
set src_dir=%~2
set priority=%~3
set category=%~4

set output_file=%ZTS_OUT%\%priority%.zts

echo [INFO] Compiling module: %name%
echo [INFO]   Source: %src_dir%
echo [INFO]   Output: %output_file%
echo [INFO]   Priority: %priority%
echo [INFO]   Category: %category%

REM Find all C source files
set obj_files=
set compile_ok=1

for %%s in ("%src_dir%\*.c") do (
    set obj=%%~dpns.o
    echo [INFO]   Compiling: %%~nxs

    %ZTS_CC% %ZTS_CFLAGS% -c -I "%ZTS_ROOT%include" -I "%LINUX_SRC%\include" -I "%LINUX_SRC%\arch\x86\include" -o "!obj!" "%%s"
    if !errorlevel! neq 0 (
        echo [ERROR] Compilation failed for %%~nxs
        set compile_ok=0
    )
    set obj_files=!obj_files! !obj!
)

if %compile_ok%==0 (
    REM Clean up object files
    for %%o in (!obj_files!) do del "%%o" 2>nul
    exit /b 1
)

REM Link into shared object (.zts format)
%ZTS_CC% %ZTS_LDFLAGS% -o "%output_file%" %obj_files%

REM Strip debug symbols
strip --strip-debug "%output_file%" 2>nul

REM Generate ZTS header (if tool is available)
if exist "%ZTS_ROOT%zts_header_tool.exe" (
    %ZTS_ROOT%zts_header_tool --name "%name%" --priority "%priority%" --category "%category%" --inject "%output_file%"
    echo [SUCCESS] ZTS header injected into %output_file%
)

echo [SUCCESS] Module compiled: %output_file%

REM Clean up object files
for %%o in (%obj_files%) do del "%%o" 2>nul

exit /b 0

REM ============================================================
REM List Available Modules
REM ============================================================
:list_modules
echo [INFO] Available modules in %ZTS_SRC%:
echo.

if not exist "%ZTS_SRC%" (
    echo   ^(no modules directory - run 'init' first^)
    goto :eof
)

for /f "delims=" %%f in ('dir /s /b "%ZTS_SRC%\*.c" 2^>nul') do (
    set module_name=%%~nf
    for %%d in ("%%~dpf.") do set dir=%%~nd
    echo   !module_name! ^(!dir!^)
)

echo.
echo [INFO] Compiled modules in %ZTS_OUT%:
echo.

if exist "%ZTS_OUT%\*.zts" (
    dir "%ZTS_OUT%\*.zts"
) else (
    echo   ^(none^)
)
goto :eof

REM ============================================================
REM Clean Build Artifacts
REM ============================================================
:clean
echo [INFO] Cleaning build artifacts...

if exist "%ZTS_OUT%" rmdir /s /q "%ZTS_OUT%"
del /s /q "%ZTS_SRC%\*.o" 2>nul
del /s /q "%ZTS_SRC%\*.zts" 2>nul

echo [SUCCESS] Clean complete
goto :eof

REM ============================================================
REM Usage
REM ============================================================
:usage
echo ZTS Module Build Toolchain
echo.
echo Usage: %0 ^<command^> [options]
echo.
echo Commands:
echo   init                    Initialize ZTS project structure
echo   build ^<module^>          Build a specific module
echo   build_all               Build all modules in modules/
echo   list                    List available modules
echo   clean                   Clean build artifacts
echo.
echo Environment Variables:
echo   ZTS_CC                  C compiler (default: gcc)
echo   LINUX_SRC               Linux kernel source path
echo.
echo Examples:
echo   %0 init
echo   %0 build tcp_cubic
echo   %0 build_all
echo   %0 list
echo   %0 clean
exit /b 1
