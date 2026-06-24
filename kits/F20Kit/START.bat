@echo off
setlocal EnableDelayedExpansion
title F20Kit launcher
cd /d "%~dp0"

REM Per-run timestamped log (keeps history; START_LAST.log is a stable alias to
REM the most-recent one for quick "where did it go" lookups).
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss" 2^>nul') do set STARTSTAMP=%%i
if not defined STARTSTAMP set STARTSTAMP=unknown_time
mkdir "%~dp0logs" 2>nul
set START_BOOT_LOG=%~dp0logs\start_%STARTSTAMP%.log
set START_BOOT_ALIAS=%~dp0START_LAST.log
(
    echo ====================================================
    echo START invoked %DATE% %TIME%
    echo Script=%~f0
    echo CWD=%CD%
    echo Args=%*
    echo ====================================================
) > "%START_BOOT_LOG%" 2>&1
copy /y "%START_BOOT_LOG%" "%START_BOOT_ALIAS%" >nul 2>&1

REM ----- elevate to admin -----
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    echo [%DATE% %TIME%] Not admin, requesting elevation... >> "%START_BOOT_LOG%" 2>&1
    powershell -NoProfile -Command "Start-Process -FilePath $env:ComSpec -ArgumentList '/k \"\"%~f0\"\"' -Verb RunAs"
    if errorlevel 1 (
        echo [!] Elevation failed or was cancelled.
        echo     Log: "%START_BOOT_LOG%"
        pause
    )
    exit /b
)

echo [%DATE% %TIME%] Running elevated. >> "%START_BOOT_LOG%" 2>&1

echo ====================================================
echo                    F20Kit Launcher
echo  Auto-fixes all blockers. Reboots if needed.
echo ====================================================
echo.

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
    echo [auto-fix] Disabling HVCI ^(Memory Integrity^)...
    reg add "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" /v Enabled /t REG_DWORD /d 0 /f >nul
    set NEED_REBOOT=1
    set ANY_FIX=1
)

REM ============================================================
REM AUTO-FIX 3: Secure Boot check (cannot be disabled from script)
REM ============================================================
for /f "delims=" %%i in ('powershell -NoProfile -Command "try { (Confirm-SecureBootUEFI) } catch { 'unsupported' }" 2^>nul') do set SB=%%i
if /i "%SB%"=="True" (
    echo.
    echo [!] Secure Boot is ENABLED - kdmap CANNOT load iqvw64e.sys.
    echo     Reboot to BIOS/UEFI - Security - Secure Boot - Disabled.
    echo     Press any key to exit ^(re-run START.bat after BIOS change^).
    pause >nul
    exit /b 1
)

REM ============================================================
REM REBOOT IF REGISTRY FIXES NEED IT (BEFORE doing anything else)
REM ============================================================
if "%NEED_REBOOT%"=="1" (
    echo.
    echo ====================================================
    echo  Applied registry fixes. REBOOT REQUIRED.
    echo  System will restart in 15 seconds.
    echo  Press Ctrl+C now to abort.
    echo ====================================================
    timeout /t 15
    shutdown /r /f /t 0 /c "F20Kit auto-applied registry fixes"
    exit /b 0
)

REM ============================================================
REM PREPARE DIAGNOSTICS FOLDER EARLY (also captures pre-clean)
REM ============================================================
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss" 2^>nul') do set DIAGDT=%%i
if not defined DIAGDT set DIAGDT=unknown_time
set DIAGROOT=%~dp0logs
set DBGDIR=%DIAGROOT%\diag_preload_%DIAGDT%
mkdir "%DIAGROOT%" 2>nul
mkdir "%DBGDIR%" 2>nul

REM ============================================================
REM AUTO-FIX 3.5: VC++ runtime prompt (only if app-local + System32 are missing)
REM ============================================================
set CRT_OK=1
for %%D in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll concrt140.dll) do (
    if not exist "%~dp0%%D" if not exist "%SystemRoot%\System32\%%D" set CRT_OK=0
)
if "%CRT_OK%"=="0" (
    echo.
    echo [!] VC++ runtime DLLs are missing. kdmap/kdunmap may close instantly.
    echo     Expected app-local DLLs next to START.bat or system-wide VC++ Redist.
    echo.
    choice /C YN /N /M "Install VC++ Redistributable now? [Y/N]: "
    if errorlevel 2 (
        echo User declined VC++ runtime install. >> "%START_BOOT_LOG%" 2>&1
        echo [!] Cannot continue without VC++ runtime.
        pause
        exit /b 1
    )
    if not exist "%~dp0install_vcredist.bat" (
        echo [!] install_vcredist.bat is missing.
        echo install_vcredist.bat missing >> "%START_BOOT_LOG%" 2>&1
        pause
        exit /b 1
    )
    call "%~dp0install_vcredist.bat"
    set VCRC=!errorlevel!
    echo VC++ install rc=!VCRC! >> "%START_BOOT_LOG%" 2>&1
    if not "!VCRC!"=="0" (
        echo [!] VC++ runtime install failed or was cancelled ^(!VCRC!^).
        pause
        exit /b 1
    )
)

REM ============================================================
REM AUTO-FIX 4: Kill running AntiCheat processes (silent)
REM ============================================================
for %%P in (vgc.exe vgtray.exe faceitclient.exe faceitservice.exe ESEAClient.exe EasyAntiCheat.exe BEService.exe BEServiceCS2.exe ricochet.exe) do (
    tasklist /fi "imagename eq %%P" 2>nul | findstr /i "%%P" >nul && (
        taskkill /f /im %%P >nul 2>&1 && echo [auto-fix] Killed %%P
        set ANY_FIX=1
    )
)

REM ============================================================
REM AUTO-FIX 5: Clean lingering iqvw64e (causes "Device\Nal in use")
REM ============================================================
sc query iqvw64e 2>nul | findstr /i "STATE" >nul && (
    sc stop iqvw64e >nul 2>&1
    sc delete iqvw64e >nul 2>&1
    echo [auto-fix] Cleaned lingering iqvw64e
    set ANY_FIX=1
)

REM ============================================================
REM AUTO-FIX 6: True unload of previous F20Driver mapping
REM ============================================================
reg query "HKLM\SOFTWARE\kdmap_tracker\F20Driver" > "%DBGDIR%\kdmap_tracker_before_start.txt" 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0unload_f20.ps1" > "%DBGDIR%\soft_unload_before_start.txt" 2>&1
set SOFTRC=!errorlevel!
if "!SOFTRC!"=="0" (
    "%~dp0kdunmap.exe" --key F20Driver --alreadyStopped > "%DBGDIR%\kdunmap_before_start.txt" 2>&1
    set KDUNMAPRC=!errorlevel!
    if "!KDUNMAPRC!"=="0" (
        echo [auto-fix] Previous F20Driver mapping fully freed
        set ANY_FIX=1
    ) else if "!KDUNMAPRC!"=="2" (
        echo [auto-fix] Previous F20Driver worker stopped; no tracker to free
        set ANY_FIX=1
    ) else (
        echo.
        echo [!] Previous F20Driver worker stopped, but kdunmap failed ^(!KDUNMAPRC!^).
        echo     Reboot before loading again.
        echo     Log: "%DBGDIR%\kdunmap_before_start.txt"
        pause
        exit /b 1
    )
) else if "!SOFTRC!"=="2" (
    reg query "HKLM\SOFTWARE\kdmap_tracker\F20Driver" >nul 2>&1 && (
        reg delete "HKLM\SOFTWARE\kdmap_tracker\F20Driver" /f >> "%DBGDIR%\kdmap_tracker_before_start.txt" 2>&1
        echo [auto-fix] Removed stale F20Driver kdmap tracker ^(no live stop event^)
        set ANY_FIX=1
    )
    echo No live F20Driver stop event found; skipped blind kdunmap. > "%DBGDIR%\kdunmap_before_start.txt"
) else if "!SOFTRC!"=="3" (
    echo.
    echo [!] Previous F20Driver did not stop cleanly.
    echo     Reboot is required before loading a new mapping.
    echo     Log: "%DBGDIR%\soft_unload_before_start.txt"
    pause
    exit /b 1
) else (
    echo.
    echo [!] Could not determine previous F20Driver state ^(soft unload rc=!SOFTRC!^).
    echo     Reboot before loading again.
    echo     Log: "%DBGDIR%\soft_unload_before_start.txt"
    pause
    exit /b 1
)

if "!ANY_FIX!"=="1" echo.

REM ============================================================
REM PRE-LOAD DIAGNOSTICS (collected before F20Driver.sys is mapped)
REM ============================================================
echo [diag] Collecting pre-load diagnostics...
echo       %DBGDIR%

(
    echo F20Kit pre-load diagnostics
    echo Generated=%DATE% %TIME%
    echo Folder=%DBGDIR%
    echo.
    echo Current user:
    whoami
    echo.
    echo Admin check:
    net session
    echo.
    echo Working dir=%CD%
) > "%DBGDIR%\summary.txt" 2>&1

(
    ver
    echo.
    REM wmic.exe is deprecated and missing on Win11 24H2+. Use CIM.
    powershell -NoProfile -Command "Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,OSArchitecture,InstallDate,LastBootUpTime | Format-List"
    echo.
    echo Build details from registry:
    reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
    echo.
    echo System info:
    systeminfo
) > "%DBGDIR%\os_systeminfo.txt" 2>&1

(
    echo === Secure Boot ===
    powershell -NoProfile -Command "try { Confirm-SecureBootUEFI } catch { $_.Exception.Message }"
    echo.
    echo === VulnerableDriverBlocklist ===
    reg query "HKLM\SYSTEM\CurrentControlSet\Control\CI\Config"
    echo.
    echo === HVCI / Memory Integrity ===
    reg query "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard" /s
    echo.
    echo === Code Integrity policy ===
    reg query "HKLM\SYSTEM\CurrentControlSet\Control\CI" /s
    echo.
    echo === bcdedit ===
    bcdedit /enum all
) > "%DBGDIR%\security_boot_ci.txt" 2>&1

(
    echo === F20Driver registry before analyzer ===
    reg query "HKLM\SOFTWARE\F20Driver"
    echo.
    echo === kdmap tracker ===
    reg query "HKLM\SOFTWARE\kdmap_tracker" /s
    echo.
    echo === Keyboard class filters ===
    reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E96B-E325-11CE-BFC1-08002BE10318}" /s
    echo.
    echo === Keyboard/HID services ===
    reg query "HKLM\SYSTEM\CurrentControlSet\Services\kbdclass" /s
    reg query "HKLM\SYSTEM\CurrentControlSet\Services\kbdhid" /s
    reg query "HKLM\SYSTEM\CurrentControlSet\Services\i8042prt" /s
    reg query "HKLM\SYSTEM\CurrentControlSet\Services\HidUsb" /s
    reg query "HKLM\SYSTEM\CurrentControlSet\Services\mouclass" /s
) > "%DBGDIR%\registry_keyboard_services.txt" 2>&1

(
    echo === Driver/service states ===
    sc query kbdclass
    sc query kbdhid
    sc query i8042prt
    sc query HidUsb
    sc query mouclass
    sc query iqvw64e
    echo.
    echo === Loaded driver modules matching keyboard/HID/F20/kdmap ===
    driverquery /v /fo list
) > "%DBGDIR%\drivers_services.txt" 2>&1

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Continue';" ^
  "'=== Keyboard PnP ===' | Out-File '%DBGDIR%\pnp_keyboard_hid.txt';" ^
  "Get-PnpDevice -Class Keyboard,HIDClass -ErrorAction SilentlyContinue | Format-List * | Out-File '%DBGDIR%\pnp_keyboard_hid.txt' -Append;" ^
  "'=== Present keyboard-related signed drivers ===' | Out-File '%DBGDIR%\signed_drivers_keyboard_hid.txt';" ^
  "Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceClass -match 'Keyboard|HIDClass' -or $_.DeviceName -match 'keyboard|hid|hotkey|razer|logitech|steelseries|asus|lenovo|gigabyte|msi|corsair' } | Sort-Object DeviceClass,DeviceName | Format-List * | Out-File '%DBGDIR%\signed_drivers_keyboard_hid.txt' -Append" > "%DBGDIR%\powershell_pnp_collect.log" 2>&1

(
    echo === File hashes ===
    powershell -NoProfile -Command "Get-FileHash '%~dp0F20Driver.sys','%~dp0analyze_kbdclass.exe','%~dp0kdmap.exe','%~dp0kdunmap.exe','%~dp0unload_f20.ps1','%~dp0cleanup_f20_state.ps1','%~dp0update_cs2_offsets.ps1','C:\Windows\System32\drivers\kbdclass.sys' -Algorithm SHA256 | Format-List"
    echo.
    echo === File version info ===
    powershell -NoProfile -Command "Get-Item '%~dp0F20Driver.sys','%~dp0analyze_kbdclass.exe','%~dp0kdmap.exe','%~dp0kdunmap.exe','%~dp0unload_f20.ps1','%~dp0cleanup_f20_state.ps1','%~dp0update_cs2_offsets.ps1','C:\Windows\System32\drivers\kbdclass.sys' | Select-Object FullName,Length,LastWriteTime,@{n='Version';e={$_.VersionInfo.FileVersion}} | Format-List"
) > "%DBGDIR%\file_hashes_versions.txt" 2>&1

copy "C:\Windows\System32\drivers\kbdclass.sys" "%DBGDIR%\kbdclass.sys" >nul 2>&1

(
    echo === Anti-cheat / input-related processes ===
    tasklist /v
    echo.
    echo === Matching processes ===
    tasklist /v | findstr /i "vgc vgtray faceit ESEA EasyAntiCheat BEService ricochet steam cs2 razer logitech steelseries corsair asus lenovo gigabyte msi"
) > "%DBGDIR%\processes.txt" 2>&1

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='SilentlyContinue';" ^
  "Get-WinEvent -FilterHashtable @{LogName='System'; Id=41,1001,1003,6008,7000,7001,7009,7026,7031,7034} -MaxEvents 80 | Select-Object TimeCreated,ProviderName,Id,LevelDisplayName,Message | Format-List | Out-File '%DBGDIR%\system_crash_service_events.txt';" ^
  "Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-Kernel-PnP'} -MaxEvents 80 | Select-Object TimeCreated,Id,LevelDisplayName,Message | Format-List | Out-File '%DBGDIR%\kernel_pnp_events.txt';" ^
  "Get-ChildItem 'C:\Windows\Minidump\*.dmp' | Sort-Object LastWriteTime -Descending | Select-Object -First 10 FullName,Length,LastWriteTime | Format-List | Out-File '%DBGDIR%\minidumps_recent.txt'" > "%DBGDIR%\powershell_events_collect.log" 2>&1

REM ============================================================
REM ANALYZE kbdclass.sys (native C++ - no Python needed)
REM ============================================================
echo [cleanup] Resetting F20Driver analyzer/runtime state before fresh analyze...
echo           CS2 offsets are preserved unless GitHub update succeeds.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cleanup_f20_state.ps1" > "%DBGDIR%\state_cleanup_before_analyzer.txt" 2>&1
type "%DBGDIR%\state_cleanup_before_analyzer.txt"
echo.

echo [offsets] Updating CS2 offsets from a2x/cs2-dumper...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0update_cs2_offsets.ps1" -CacheDir "%DBGDIR%" > "%DBGDIR%\cs2_offsets_update.txt" 2>&1
set OFFSRC=!errorlevel!
type "%DBGDIR%\cs2_offsets_update.txt"
if not "!OFFSRC!"=="0" (
    echo [i] GitHub offsets update failed - driver will use built-in fallback offsets.
)
reg query "HKLM\SOFTWARE\F20Driver" > "%DBGDIR%\registry_f20_after_offsets.txt" 2>&1
echo.

echo [1/2] Analyzing kbdclass.sys (native)...
cmd /c ""%~dp0analyze_kbdclass.exe" > "%DBGDIR%\analyze_output.txt" 2>&1"
set ANALYZRC=!errorlevel!
type "%DBGDIR%\analyze_output.txt"
reg query "HKLM\SOFTWARE\F20Driver" > "%DBGDIR%\registry_f20_after_analyzer.txt" 2>&1
if not "!ANALYZRC!"=="0" if not "!ANALYZRC!"=="2" (
    echo [!] analyze_kbdclass returned !ANALYZRC! - cannot proceed
    echo [diag] Pre-load diagnostics saved in:
    echo        %DBGDIR%
    pause
    exit /b 1
)
if "!ANALYZRC!"=="2" (
    echo [i] No PDB symbol or signature matched - driver will run in monitor-only mode.
    echo     ^(No BSOD risk, but F20/Num inject will be disabled.^)
    echo     Send a diag_collect.bat zip to dev to inspect symbols/signatures.
    echo.
)

(
    echo OffsetUpdateExitCode=!OFFSRC!
    echo AnalyzeExitCode=!ANALYZRC!
    echo DiagnosticsFolder=%DBGDIR%
    echo ReadyToLoadDriver=1
) >> "%DBGDIR%\summary.txt"

powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%DBGDIR%\*' -DestinationPath '%DBGDIR%.zip' -Force" >nul 2>&1

echo [diag] Pre-load diagnostics saved:
echo        %DBGDIR%
if exist "%DBGDIR%.zip" echo        %DBGDIR%.zip
echo.

REM ============================================================
REM LOAD DRIVER via kdmap (--indPages, tracked for clean unload)
REM ============================================================
echo [2/2] Loading driver via kdmap (--indPages, tracked)...
cmd /c ""%~dp0kdmap.exe" --key F20Driver --stopEvent "Global\F20DriverStop" --indPages "%~dp0F20Driver.sys" > "%DBGDIR%\kdmap_output.txt" 2>&1"
type "%DBGDIR%\kdmap_output.txt"
if errorlevel 1 (
    echo.
    echo [!] kdmap failed. See output above.
    echo     Common reasons:
    echo       - 0xC0000603: VulnerableDriverBlocklist still active ^(reboot?^)
    echo       - 0xC0000022 / 0xC000009A: AntiCheat / Defender blocking
    echo       - "Device\Nal in use": old iqvw64e session, reboot may help
    echo.
    echo [diag] Logs saved in:
    echo        %DBGDIR%
    pause
    exit /b 1
)
echo.

echo ====================================================
echo  Driver loaded. In CS2 console:
echo       WRITE IN CONSOLE UNBIND F20
echo  On every kill: F20 held 2.5 sec + random Num 0-9 tap.
echo  Make sure NumLock=ON on your keyboard before playing.
echo.
echo  STOP: run STOP.bat (safe stop, then true unload if confirmed).
echo ====================================================
echo.
pause
