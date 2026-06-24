@echo off
REM ============================================================================
REM install_vcredist.bat (F20Kit)
REM
REM Optional helper for machines where VC++ runtime DLLs were deleted,
REM quarantined, or are not installed system-wide.
REM
REM Default mode:
REM   Microsoft VC++ 2015-2022 x64 Redistributable
REM   https://aka.ms/vs/17/release/vc_redist.x64.exe
REM
REM Optional AIO mode:
REM   abbodi1406 VisualCppRedist_AIO_x86_x64.exe from latest GitHub release.
REM   CLI used: /y  (install all packages with progress, no start/finish prompt)
REM
REM Normally NOT needed: the kit ships app-local CRT DLLs next to kdmap/kdunmap.
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

set "INSTALL_MODE=official"
if /i "%~1"=="/aio" set "INSTALL_MODE=aio"
if /i "%~1"=="aio" set "INSTALL_MODE=aio"

if /i "%INSTALL_MODE%"=="official" (
    echo.
    echo VC++ runtime installer
    echo.
    echo   M - Microsoft VC++ 2015-2022 x64 Redistributable ^(recommended^)
    echo   A - VisualCppRedist AIO latest from abbodi1406 GitHub release
    echo   Q - cancel
    echo.
    choice /C MAQ /N /M "Choose installer [M/A/Q]: "
    if errorlevel 3 exit /b 2
    if errorlevel 2 set "INSTALL_MODE=aio"
)

if /i "%INSTALL_MODE%"=="aio" goto INSTALL_AIO

set "VCREDIST_URL=https://aka.ms/vs/17/release/vc_redist.x64.exe"
set "VCREDIST_DST=%TEMP%\vc_redist.x64.exe"

if exist "%SystemRoot%\System32\msvcp140.dll" (
    echo MSVCP140.dll already present in System32. Nothing to install.
    timeout /t 3 >nul
    exit /b 0
)

echo.
echo VC++ Redistributable not detected system-wide.
echo Downloading from %VCREDIST_URL% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference='Stop';" ^
    "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13;" ^
    "Invoke-WebRequest -UseBasicParsing -Uri '%VCREDIST_URL%' -OutFile '%VCREDIST_DST%' -TimeoutSec 90"
if not exist "%VCREDIST_DST%" (
    echo [!] Download failed. Check internet / firewall.
    echo     URL: %VCREDIST_URL%
    pause
    exit /b 1
)

echo.
echo Installing Microsoft VC++ Redistributable ^(passive, no reboot^)...
"%VCREDIST_DST%" /install /passive /norestart
set RC=%errorlevel%
del /q "%VCREDIST_DST%" 2>nul

if "%RC%"=="0"    (echo [+] VC++ Redistributable installed.       & exit /b 0)
if "%RC%"=="1638" (echo [+] Newer version already installed.       & exit /b 0)
if "%RC%"=="3010" (echo [+] Installed, reboot required to finalize. & exit /b 0)
echo [!] Installer returned code %RC%. See Windows Event Log for details.
pause
exit /b %RC%

:INSTALL_AIO
set "AIO_DST=%TEMP%\VisualCppRedist_AIO_x86_x64.exe"
echo.
echo Downloading latest VisualCppRedist AIO from abbodi1406 GitHub release...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference='Stop';" ^
    "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13;" ^
    "$headers=@{'User-Agent'='NL_Drive_CS2'};" ^
    "$rel=Invoke-RestMethod -Headers $headers -Uri 'https://api.github.com/repos/abbodi1406/vcredist/releases/latest';" ^
    "$asset=$rel.assets | Where-Object { $_.name -eq 'VisualCppRedist_AIO_x86_x64.exe' } | Select-Object -First 1;" ^
    "if(-not $asset){ throw 'VisualCppRedist_AIO_x86_x64.exe not found in latest release' };" ^
    "Write-Host ('Latest AIO release: ' + $rel.tag_name);" ^
    "Invoke-WebRequest -UseBasicParsing -Headers $headers -Uri $asset.browser_download_url -OutFile '%AIO_DST%' -TimeoutSec 180"
if not exist "%AIO_DST%" (
    echo [!] AIO download failed.
    pause
    exit /b 1
)

echo.
echo Installing VisualCppRedist AIO ^(all packages, progress UI^)...
"%AIO_DST%" /y
set RC=%errorlevel%
del /q "%AIO_DST%" 2>nul

if "%RC%"=="0"    (echo [+] VisualCppRedist AIO installed.          & exit /b 0)
if "%RC%"=="3010" (echo [+] Installed, reboot required to finalize. & exit /b 0)
echo [!] AIO installer returned code %RC%.
pause
exit /b %RC%
