@echo off
setlocal

REM Uninstall EGoTouch ARM64 KMDF package

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Administrator privileges required. Please run as Administrator.
    exit /b 1
)

set DRIVER_INF=EGoTouchKm.inf

echo [INFO] Enumerating third-party drivers...
for /f "tokens=1,* delims=:" %%A in ('pnputil /enum-drivers ^| findstr /i /c:"Published Name" /c:"Original Name"') do (
    set key=%%A
    set val=%%B
)

echo [INFO] Attempting package removal by INF name...
pnputil /delete-driver %DRIVER_INF% /uninstall /force
if %errorlevel% neq 0 (
    echo [WARN] Could not remove by literal INF name. You may need to remove oemXX.inf manually.
    exit /b 2
)

echo [OK] Driver package removed.
exit /b 0
