@echo off
REM ============================================================================
REM install_vcredist.bat
REM
REM Optional helper: downloads and installs Microsoft Visual C++ 2015-2022
REM Redistributable (x64) if it is not already on the system.
REM
REM Normally NOT needed: kits/.../bin/ already ships msvcp140.dll +
REM vcruntime140.dll + vcruntime140_1.dll + concrt140.dll app-local, so the
REM exes here run on stock Windows 10/11 without any system install.
REM
REM Use this script if:
REM   - the app-local DLLs got deleted/quarantined by AV,
REM   - you want VC Redist system-wide so other tools also work,
REM   - run.bat reports "VC runtime missing".
REM
REM Source URL: https://aka.ms/vs/17/release/vc_redist.x64.exe
REM   This is the official Microsoft permalink to the latest VC++ 2015-2022
REM   Redistributable. No URL shortener / tinyurl in this chain.
REM ============================================================================
setlocal EnableDelayedExpansion
cd /d "%~dp0"
title VC++ Redistributable installer

REM Re-launch elevated if needed (msiexec requires admin to register globally).
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights to install VC++ Redistributable...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "VCREDIST_URL=https://aka.ms/vs/17/release/vc_redist.x64.exe"
set "VCREDIST_DST=%TEMP%\vc_redist.x64.exe"

REM Check if MSVCP140.dll is already resolvable from PATH / system32.
where /q msvcp140.dll
if %errorlevel%==0 (
    echo MSVCP140.dll already present on this system. Nothing to install.
    timeout /t 3 >nul
    exit /b 0
)

echo.
echo VC++ Redistributable not detected on this system.
echo Downloading from %VCREDIST_URL% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13;" ^
    "Invoke-WebRequest -UseBasicParsing -Uri '%VCREDIST_URL%' -OutFile '%VCREDIST_DST%' -TimeoutSec 60"
if not exist "%VCREDIST_DST%" (
    echo [!] Download failed. Check internet / firewall.
    echo     URL: %VCREDIST_URL%
    pause
    exit /b 1
)

echo.
echo Installing silently (no reboot)...
"%VCREDIST_DST%" /install /passive /norestart
set RC=%errorlevel%
del /q "%VCREDIST_DST%" 2>nul

REM Microsoft installer exit codes:
REM   0    - success
REM   1638 - newer version already installed (treat as OK)
REM   3010 - success, reboot required
if "%RC%"=="0"    (echo [+] VC++ Redistributable installed.       & exit /b 0)
if "%RC%"=="1638" (echo [+] Newer version already installed.       & exit /b 0)
if "%RC%"=="3010" (echo [+] Installed, reboot required to finalize. & exit /b 0)
echo [!] Installer returned code %RC%. See Windows Event Log for details.
pause
exit /b %RC%
