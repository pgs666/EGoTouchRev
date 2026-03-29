@echo off
:: EGoTouchService Install Script
:: Requires Administrator privileges

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Administrator privileges required. Right-click "Run as administrator".
    pause
    exit /b 1
)

echo === EGoTouchService Install ===
echo.

:: Create data directory
if not exist "C:\ProgramData\EGoTouchRev" mkdir "C:\ProgramData\EGoTouchRev"
if not exist "C:\ProgramData\EGoTouchRev\logs" mkdir "C:\ProgramData\EGoTouchRev\logs"

:: Install service
sc create EGoTouchService binPath= "%~dp0EGoTouchService.exe" start= auto
if %errorlevel% neq 0 (
    echo [WARN] Service may already exist or install failed.
)

:: Failure recovery: restart after 5s / 10s / 30s, reset counter after 24h
sc failure EGoTouchService reset= 86400 actions= restart/5000/restart/10000/restart/30000

:: Description
sc description EGoTouchService "EGoTouch Capacitive Touch Controller Driver Service"

echo.
echo [OK] Install complete.
echo     Start:  sc start EGoTouchService
echo     Stop:   sc stop EGoTouchService
echo.
echo Please copy config.ini to C:\ProgramData\EGoTouchRev\config.ini
echo.
pause
