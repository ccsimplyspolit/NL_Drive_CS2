@echo off
setlocal EnableDelayedExpansion
title IsValveDS Spoofer launcher
cd /d "%~dp0"

set LOG=%~dp0run_LAST.log
(
    echo ====================================================
    echo run.bat invoked %DATE% %TIME%
    echo CWD=%CD%
    echo ====================================================
) > "%LOG%" 2>&1

REM ----- elevate to admin -----
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    echo Not admin, requesting elevation... >> "%LOG%" 2>&1
    powershell -NoProfile -Command "Start-Process -FilePath $env:ComSpec -ArgumentList '/k \"\"%~f0\"\"' -Verb RunAs"
    if errorlevel 1 (
        echo [!] Elevation failed or was cancelled.
        echo     Log: "%LOG%"
        pause
    )
    exit /b
)

echo ====================================================
echo          IsValveDS Spoofer  -  Launcher
echo  Auto-fixes all blockers. Reboots if needed.
echo ====================================================
echo.

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss" 2^>nul') do set RUNDT=%%i
if not defined RUNDT set RUNDT=unknown_time
set LOGROOT=%~dp0logs
set DBGDIR=%LOGROOT%\diag_preload_%RUNDT%
mkdir "%LOGROOT%" 2>nul
mkdir "%DBGDIR%" 2>nul
(
    echo IsValveDS pre-load diagnostics
    echo Generated=%DATE% %TIME%
    echo Folder=%DBGDIR%
    echo Working dir=%CD%
    echo Computer=%COMPUTERNAME%
    echo User=%USERNAME%
) > "%DBGDIR%\summary.txt" 2>&1
echo [diag] Collecting pre-load diagnostics...
echo DiagnosticsFolder=%DBGDIR% >> "%LOG%" 2>&1

set NEED_REBOOT=0
set ANY_FIX=0

REM ============================================================
REM AUTO-FIX 1: VulnerableDriverBlocklist
REM ============================================================
set VDB=
for /f "tokens=3" %%i in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\CI\Config" /v VulnerableDriverBlocklistEnable 2^>nul ^| findstr "VulnerableDriverBlocklistEnable"') do set VDB=%%i
if not "%VDB%"=="0x0" (
    echo [auto-fix] Disabling VulnerableDriverBlocklist...
    reg add "HKLM\SYSTEM\CurrentControlSet\Control\CI\Config" /v VulnerableDriverBlocklistEnable /t REG_DWORD /d 0 /f >nul
    set NEED_REBOOT=1
    set ANY_FIX=1
)

REM ============================================================
REM AUTO-FIX 2: HVCI / Memory Integrity
REM ============================================================
set HVCI=
for /f "tokens=3" %%i in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" /v Enabled 2^>nul ^| findstr /i "Enabled"') do set HVCI=%%i
if defined HVCI if not "%HVCI%"=="0x0" (
    echo [auto-fix] Disabling HVCI...
    reg add "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" /v Enabled /t REG_DWORD /d 0 /f >nul
    set NEED_REBOOT=1
    set ANY_FIX=1
)

REM ============================================================
REM Secure Boot (cannot auto-disable, abort if enabled)
REM ============================================================
for /f "delims=" %%i in ('powershell -NoProfile -Command "try { (Confirm-SecureBootUEFI) } catch { 'unsupported' }" 2^>nul') do set SB=%%i
if /i "%SB%"=="True" (
    echo.
    echo [!] Secure Boot is ENABLED - kdmap cannot load iqvw64e.sys.
    echo     Reboot to BIOS/UEFI - Security - Secure Boot - Disabled.
    pause
    exit /b 1
)

REM ============================================================
REM Reboot if registry fixes were applied
REM ============================================================
if "%NEED_REBOOT%"=="1" (
    echo.
    echo ====================================================
    echo  Applied registry fixes. REBOOT REQUIRED.
    echo  System will restart in 15 seconds.
    echo  Press Ctrl+C now to abort.
    echo ====================================================
    timeout /t 15
    shutdown /r /f /t 0 /c "IsValveDS auto-applied registry fixes"
    exit /b 0
)

if not exist kdmap.exe (
    echo [!] kdmap.exe is missing in this folder.
    echo Missing kdmap.exe >> "%LOG%" 2>&1
    echo Missing kdmap.exe >> "%DBGDIR%\summary.txt" 2>&1
    pause
    exit /b 1
)
if not exist kdunmap.exe (
    echo [!] kdunmap.exe is missing in this folder.
    echo Missing kdunmap.exe >> "%LOG%" 2>&1
    echo Missing kdunmap.exe >> "%DBGDIR%\summary.txt" 2>&1
    pause
    exit /b 1
)
if not exist IsValveDS_Driver.sys (
    echo [!] IsValveDS_Driver.sys is missing in this folder.
    echo Missing IsValveDS_Driver.sys >> "%LOG%" 2>&1
    echo Missing IsValveDS_Driver.sys >> "%DBGDIR%\summary.txt" 2>&1
    pause
    exit /b 1
)

(
    echo === Windows version ===
    ver
    echo.
    echo === systeminfo ===
    systeminfo
    echo.
    echo === CurrentVersion registry ===
    reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
) > "%DBGDIR%\os_systeminfo.txt" 2>&1

(
    echo === Secure Boot ===
    powershell -NoProfile -Command "try { Confirm-SecureBootUEFI } catch { $_.Exception.Message }"
    echo.
    echo === CI\Config ===
    reg query "HKLM\SYSTEM\CurrentControlSet\Control\CI\Config"
    echo.
    echo === HVCI ===
    reg query "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity"
    echo.
    echo === bcdedit ===
    bcdedit /enum
) > "%DBGDIR%\security_boot_ci.txt" 2>&1

(
    echo === IsValveDS registry before offsets ===
    reg query "HKLM\SOFTWARE\IsValveDS"
    echo.
    echo === kdmap tracker before start ===
    reg query "HKLM\SOFTWARE\kdmap_tracker"
) > "%DBGDIR%\registry_before_start.txt" 2>&1

(
    echo === File hashes ===
    powershell -NoProfile -Command "Get-FileHash '%~dp0IsValveDS_Driver.sys','%~dp0IsValveDS_Console.exe','%~dp0kdmap.exe','%~dp0kdunmap.exe','%~dp0unload_isvalveds.ps1','%~dp0update_isvalveds_offsets.ps1' -Algorithm SHA256 | Format-List"
    echo.
    echo === File version info ===
    powershell -NoProfile -Command "Get-Item '%~dp0IsValveDS_Driver.sys','%~dp0IsValveDS_Console.exe','%~dp0kdmap.exe','%~dp0kdunmap.exe','%~dp0unload_isvalveds.ps1','%~dp0update_isvalveds_offsets.ps1' | Select-Object FullName,Length,LastWriteTime,@{n='Version';e={$_.VersionInfo.FileVersion}} | Format-List"
) > "%DBGDIR%\file_hashes_versions.txt" 2>&1

(
    echo === Services ===
    sc query iqvw64e
    sc query type= driver
    echo.
    echo === Driver query ===
    driverquery /v /fo list
) > "%DBGDIR%\drivers_services.txt" 2>&1

(
    echo === Processes ===
    tasklist /v
    echo.
    echo === Matching processes ===
    tasklist /v | findstr /i "cs2 steam vgc vgtray faceit ESEA EasyAntiCheat BEService ricochet defender MsMpEng"
) > "%DBGDIR%\processes.txt" 2>&1

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='SilentlyContinue';" ^
  "Get-WinEvent -FilterHashtable @{LogName='System'; Id=41,1001,1003,6008,7000,7001,7009,7026,7031,7034} -MaxEvents 80 | Select-Object TimeCreated,ProviderName,Id,LevelDisplayName,Message | Format-List | Out-File '%DBGDIR%\system_crash_service_events.txt';" ^
  "Get-ChildItem 'C:\Windows\Minidump\*.dmp' | Sort-Object LastWriteTime -Descending | Select-Object -First 10 FullName,Length,LastWriteTime | Format-List | Out-File '%DBGDIR%\minidumps_recent.txt'" > "%DBGDIR%\powershell_events_collect.log" 2>&1

REM ============================================================
REM AUTO-FIX 3: Stop previous console + kill stray AC processes
REM ============================================================
taskkill /f /im IsValveDS_Console.exe >nul 2>&1
for %%P in (vgc.exe faceitclient.exe faceitservice.exe ESEAClient.exe EasyAntiCheat.exe BEService.exe) do (
    tasklist /fi "imagename eq %%P" 2>nul | findstr /i "%%P" >nul && (
        taskkill /f /im %%P >nul 2>&1 && echo [auto-fix] Killed %%P
        set ANY_FIX=1
    )
)

REM ============================================================
REM AUTO-FIX 4: Clean lingering iqvw64e
REM ============================================================
sc query iqvw64e 2>nul | findstr /i "STATE" >nul && (
    sc stop iqvw64e >nul 2>&1
    sc delete iqvw64e >nul 2>&1
    echo [auto-fix] Cleaned lingering iqvw64e
    set ANY_FIX=1
)

REM ============================================================
REM AUTO-FIX 5: Safe unload of previous IsValveDS_Driver mapping
REM ============================================================
echo [auto-fix] Checking for previous IsValveDS_Driver mapping...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0unload_isvalveds.ps1" > "%DBGDIR%\unload_before_load.log" 2>&1
set SOFTRC=!errorlevel!
type "%DBGDIR%\unload_before_load.log"
type "%DBGDIR%\unload_before_load.log" >> "%LOG%"

if "!SOFTRC!"=="0" (
    echo [auto-fix] Worker exit confirmed. Freeing previous tracked mapping...
    "%~dp0kdunmap.exe" --key IsValveDS_Driver --alreadyStopped > "%DBGDIR%\kdunmap_before_load.log" 2>&1
    set KDUNMAPRC=!errorlevel!
    type "%DBGDIR%\kdunmap_before_load.log"
    type "%DBGDIR%\kdunmap_before_load.log" >> "%LOG%"
    if "!KDUNMAPRC!"=="0" (
        echo [auto-fix] Previous IsValveDS_Driver mapping fully freed
        set ANY_FIX=1
    ) else if "!KDUNMAPRC!"=="2" (
        echo [auto-fix] Previous driver stopped; no tracked mapping record found
    ) else (
        echo [!] kdunmap failed rc=!KDUNMAPRC!.
        echo     Reboot before loading again.
        echo kdunmap failed rc=!KDUNMAPRC! >> "%LOG%" 2>&1
        pause
        exit /b 1
    )
) else if "!SOFTRC!"=="2" (
    reg query "HKLM\SOFTWARE\kdmap_tracker\IsValveDS_Driver" >nul 2>&1 && (
        reg delete "HKLM\SOFTWARE\kdmap_tracker\IsValveDS_Driver" /f >> "%LOG%" 2>&1
        echo [auto-fix] Removed stale IsValveDS_Driver tracker ^(no live stop event^)
        set ANY_FIX=1
    )
) else (
    echo [!] Previous IsValveDS_Driver did not stop cleanly ^(rc=!SOFTRC!^).
    echo     NOT running kdunmap, because freeing live code can BSOD.
    echo     Reboot before loading again.
    echo soft unload failed rc=!SOFTRC! >> "%LOG%" 2>&1
    pause
    exit /b 1
)

if "!ANY_FIX!"=="1" echo.

REM ============================================================
REM UPDATE CS2 OFFSETS (F20Kit-style: download before driver load)
REM ============================================================
echo [offsets] Updating IsValveDS CS2 offsets from a2x/cs2-dumper...
set OFFRC=99
if exist "%~dp0update_isvalveds_offsets.ps1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0update_isvalveds_offsets.ps1" -CacheDir "%DBGDIR%" > "%DBGDIR%\offsets_update.log" 2>&1
    set OFFRC=!errorlevel!
    type "%DBGDIR%\offsets_update.log"
    type "%DBGDIR%\offsets_update.log" >> "%LOG%"
    reg query "HKLM\SOFTWARE\IsValveDS" > "%DBGDIR%\registry_isvalveds_after_offsets.log" 2>&1
    type "%DBGDIR%\registry_isvalveds_after_offsets.log" >> "%LOG%"
    if not "!OFFRC!"=="0" (
        echo [i] GitHub offsets update failed - driver will use registry if valid, otherwise built-in fallback.
        echo offsets update failed rc=!OFFRC! >> "%LOG%" 2>&1
    )
) else (
    echo [i] update_isvalveds_offsets.ps1 is missing - driver will use registry if valid, otherwise built-in fallback.
    echo update_isvalveds_offsets.ps1 missing >> "%LOG%" 2>&1
    set OFFRC=98
)
echo.

(
    echo OffsetUpdateExitCode=!OFFRC!
    echo DiagnosticsFolder=%DBGDIR%
    echo ReadyToLoadDriver=1
) >> "%DBGDIR%\summary.txt"

powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%DBGDIR%\*' -DestinationPath '%DBGDIR%.zip' -Force" >nul 2>&1

echo [diag] Pre-load diagnostics saved:
echo        %DBGDIR%
if exist "%DBGDIR%.zip" echo        %DBGDIR%.zip
echo.

REM ============================================================
REM Verify cs2.exe is running (warn but don't block)
REM ============================================================
tasklist /fi "imagename eq cs2.exe" 2>nul | findstr /i "cs2.exe" >nul || (
    echo [!] cs2.exe is NOT running.
    echo     IsValveDS will sit in 'cs2 not found' until you launch the game.
    echo     Press any key to continue anyway, or close window and start CS2 first.
    pause >nul
)

REM ============================================================
REM Map fresh driver via kdmap (--indPages, tracked)
REM ============================================================
echo.
echo [1/2] Mapping IsValveDS_Driver.sys via kdmap (--indPages, tracked)...
"%~dp0kdmap.exe" --key IsValveDS_Driver --stopEvent "Global\IsValveDSStop" --indPages "%~dp0IsValveDS_Driver.sys" > "%DBGDIR%\kdmap_load.log" 2>&1
set MAPRC=!errorlevel!
type "%DBGDIR%\kdmap_load.log"
type "%DBGDIR%\kdmap_load.log" >> "%LOG%"
if not "!MAPRC!"=="0" (
    echo.
    echo [!] kdmap failed rc=!MAPRC!. See output above.
    echo     Common reasons:
    echo       - 0xC0000603: VulnerableDriverBlocklist still active ^(reboot?^)
    echo       - 0xC0000022 / 0xC000009A: AntiCheat / Defender blocking
    echo       - "Device\Nal in use": old iqvw64e session, reboot may help
    echo     Log: "%LOG%"
    echo     Diagnostics: "%DBGDIR%"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%DBGDIR%\*' -DestinationPath '%DBGDIR%.zip' -Force" >nul 2>&1
    pause
    exit /b 1
)

REM ============================================================
REM Run console (auto-polls every 3s)
REM ============================================================
echo.
echo [2/2] Starting console (auto-poll every 3s, 'h' for commands)...
echo     Console writes detailed events to IsValveDS_Console.log
echo.
REM Two things to note:
REM   1. We do NOT redirect stdin (no `< NUL`) - console needs a real TTY for
REM      its ReadConsoleA loop. Redirecting stdin here would kill it.
REM   2. We capture the exit code so the post-run summary can mention it.
"%~dp0IsValveDS_Console.exe"
set CONSOLE_RC=%errorlevel%
echo.
echo Console exited with code %CONSOLE_RC%.
echo run log:     "%LOG%"
echo console log: "%~dp0IsValveDS_Console.log"
if not "%CONSOLE_RC%"=="0" (
    echo [!] Console reported a non-zero exit.
    echo     Open IsValveDS_Console.log for full diagnostics and send the
    echo     last ~50 lines to dev. The log captures every WinAPI call,
    echo     errno via FormatMessage, and an unhandled-exception filter.
)
pause
