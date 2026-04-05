@echo off
setlocal

REM Build ARM64 KMDF driver from Visual Studio Build Tools

set SCRIPT_DIR=%~dp0
set REPO_ROOT=%SCRIPT_DIR%..
set SLN=%REPO_ROOT%\KernelDriver\EGoTouchKm\EGoTouchKm.sln

if not exist "%SLN%" (
    echo [ERROR] Solution not found: %SLN%
    exit /b 1
)

where msbuild >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] msbuild not found in PATH. Open "x64 Native Tools / Developer Command Prompt for VS".
    exit /b 2
)

echo [INFO] Building EGoTouchKm (Release^|ARM64)...
msbuild "%SLN%" /t:Build /p:Configuration=Release /p:Platform=ARM64
if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    exit /b %errorlevel%
)

echo [OK] Build complete.
exit /b 0
