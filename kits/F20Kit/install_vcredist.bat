@echo off
REM ============================================================================
REM install_vcredist.bat (F20Kit)
REM
REM Optional helper: downloads and installs Microsoft Visual C++ 2015-2022
REM Redistributable (x64) if it is not already on the system.
REM
REM Normally NOT needed: kits/F20Kit/ ships msvcp140.dll + vcruntime140.dll +
REM vcruntime140_1.dll + concrt140.dll app-local, so kdmap.exe / kdunmap.exe
REM run on stock Windows 10/11 without any system install.
REM
REM Source URL: https://aka.ms/vs/17/release/vc_redist.x64.exe (Microsoft).
REM ============================================================================
setlocal EnableDelayedExpansion
cd /d "%~dp0"
title VC++ Redistributable installer

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights to install VC++ Redistributable...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "VCREDIST_URL=https://aka.ms/vs/17/release/vc_redist.x64.exe"
set "VCREDIST_DST=%TEMP%\vc_redist.x64.exe"

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

if "%RC%"=="0"    (echo [+] VC++ Redistributable installed.       & exit /b 0)
if "%RC%"=="1638" (echo [+] Newer version already installed.       & exit /b 0)
if "%RC%"=="3010" (echo [+] Installed, reboot required to finalize. & exit /b 0)
echo [!] Installer returned code %RC%. See Windows Event Log for details.
pause
exit /b %RC%
