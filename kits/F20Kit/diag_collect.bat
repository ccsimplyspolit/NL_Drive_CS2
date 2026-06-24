@echo off
title F20Kit diagnostic collector
cd /d "%~dp0"

REM ----- elevate to admin -----
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

echo ====================================================
echo   F20Kit diagnostic data collector
echo   (run this on the machine that BSODs and send back
echo    the resulting f20_diag_*.zip)
echo ====================================================
echo.

REM ----- timestamp for output folder -----
REM wmic.exe is deprecated and missing on Win11 24H2+. Use PowerShell instead.
for /f "usebackq tokens=*" %%a in (`powershell -NoProfile -Command "Get-Date -Format 'yyyyMMdd_HHmmss'"`) do set DT=%%a
if not defined DT set DT=unknown_time
set OUT=f20_diag_%DT%
mkdir "%OUT%" 2>nul

echo [1/4] Collecting OS info...
powershell -NoProfile -Command "Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,OSArchitecture | Format-List" > "%OUT%\osinfo.txt" 2>&1
ver >> "%OUT%\osinfo.txt"
echo. >> "%OUT%\osinfo.txt"
echo Build details from registry: >> "%OUT%\osinfo.txt"
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v ProductName >> "%OUT%\osinfo.txt" 2>&1
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v DisplayVersion >> "%OUT%\osinfo.txt" 2>&1
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentBuild >> "%OUT%\osinfo.txt" 2>&1
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v UBR >> "%OUT%\osinfo.txt" 2>&1

echo [2/4] Copying kbdclass.sys (THIS IS THE KEY FILE WE NEED)...
copy "C:\Windows\System32\drivers\kbdclass.sys" "%OUT%\kbdclass.sys" >nul
if errorlevel 1 (
    echo [!] Failed to copy kbdclass.sys
) else (
    powershell -NoProfile -Command "Get-FileHash '%OUT%\kbdclass.sys' -Algorithm SHA256 | Select-Object -ExpandProperty Hash" > "%OUT%\kbdclass.sha256.txt"
    powershell -NoProfile -Command "Get-FileHash '%OUT%\kbdclass.sys' -Algorithm SHA256" >> "%OUT%\osinfo.txt"
)

echo [3/4] Running analyze_kbdclass.exe (captures candidate RVAs)...
"%~dp0analyze_kbdclass.exe" > "%OUT%\analyze_output.txt" 2>&1
if errorlevel 1 (
    echo     analyzer returned non-zero exit
)

echo     Dumping current registry state (what driver would read on load):
reg query "HKLM\SOFTWARE\F20Driver" > "%OUT%\registry_state.txt" 2>&1

echo [4/4] Checking last bugcheck info...
powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-WER-SystemErrorReporting','BugCheck','Microsoft-Windows-Kernel-Power'} -MaxEvents 20 -ErrorAction SilentlyContinue | Select-Object TimeCreated,Id,LevelDisplayName,Message | Format-List" > "%OUT%\bugcheck_events.txt" 2>&1
REM tail is not standard on Windows; use PowerShell Select-Object -Last.
powershell -NoProfile -Command "Get-ChildItem 'C:\Windows\Minidump\*.dmp' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 5 -ExpandProperty Name" > "%OUT%\minidumps_recent.txt" 2>&1

echo.
echo [*] Zipping...
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath '%OUT%.zip' -Force"
if exist "%OUT%.zip" (
    echo.
    echo ====================================================
    echo   Done. Send this file:
    echo     %CD%\%OUT%.zip
    echo ====================================================
) else (
    echo [!] Zip failed. The raw folder is here:
    echo     %CD%\%OUT%\
)
echo.
pause
