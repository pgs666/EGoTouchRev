@echo off
setlocal

REM EGoTouch ARM64 KMDF Driver one-click install script
REM Requires Administrator privileges and a signed CAT file.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Administrator privileges required. Please run as Administrator.
    exit /b 1
)

set SCRIPT_DIR=%~dp0
set REPO_ROOT=%SCRIPT_DIR%..
set DRIVER_DIR=%REPO_ROOT%\KernelDriver\EGoTouchKm

set DRIVER_INF=%DRIVER_DIR%\EGoTouchKm.inf
set DRIVER_SYS=%DRIVER_DIR%\ARM64\Release\EGoTouchKm.sys
set DRIVER_CAT=%DRIVER_DIR%\ARM64\Release\EGoTouchKm.cat

if not exist "%DRIVER_INF%" (
    echo [ERROR] Missing INF: %DRIVER_INF%
    exit /b 2
)

if not exist "%DRIVER_SYS%" (
    set DRIVER_SYS=%DRIVER_DIR%\x64\Release\EGoTouchKm.sys
)

if not exist "%DRIVER_CAT%" (
    set DRIVER_CAT=%DRIVER_DIR%\x64\Release\EGoTouchKm.cat
)

if not exist "%DRIVER_SYS%" (
    echo [ERROR] Missing SYS binary. Build the driver first.
    exit /b 3
)

if not exist "%DRIVER_CAT%" (
    echo [WARN] Missing CAT file. PnP install may fail on secure boot systems.
)

echo [INFO] Staging driver package...
pnputil /add-driver "%DRIVER_INF%" /install
if %errorlevel% neq 0 (
    echo [ERROR] pnputil install failed.
    exit /b 4
)

echo [INFO] Enumerating root device instance...
pnputil /scan-devices >nul 2>&1

echo [OK] EGoTouchKm driver package installed.
exit /b 0
