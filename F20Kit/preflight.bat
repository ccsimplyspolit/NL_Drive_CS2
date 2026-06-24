@echo off
chcp 65001 >nul 2>&1
title F20Kit preflight
cd /d "%~dp0"

REM ----- elevate to admin -----
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

echo ====================================================
echo   PREFLIGHT - System checks for kdmapper-based kits
echo   START.bat auto-fixes most of these. This script
echo   just SHOWS the state without changes.
echo ====================================================
echo.

set ANY_PROBLEM=0
set NEED_REBOOT=0

REM ============================================================
REM 1. analyze_kbdclass.exe (native, replaces old Python script)
REM ============================================================
echo [1/8] Native analyzer presence...
if exist "%~dp0analyze_kbdclass.exe" (
    echo   [OK]   analyze_kbdclass.exe present ^(no Python needed^)
) else (
    echo   [FAIL] analyze_kbdclass.exe missing - did you forget to copy it?
    set ANY_PROBLEM=1
)
echo.

REM ============================================================
REM 2. Secure Boot - cannot disable from script, only detect
REM ============================================================
echo [2/8] Secure Boot status...
for /f "tokens=*" %%i in ('powershell -NoProfile -Command "try { (Confirm-SecureBootUEFI) } catch { 'unsupported' }" 2^>nul') do set SB=%%i
if /i "%SB%"=="True" (
    echo   [FAIL] Secure Boot is ENABLED - kdmapper cannot load iqvw64e.sys
    echo          Disable it via BIOS/UEFI:
    echo          - reboot into BIOS (Del/F2/F10 - depends on motherboard)
    echo          - Security -^> Secure Boot -^> Disabled
    echo          - Save ^& Exit
    set ANY_PROBLEM=1
) else if /i "%SB%"=="False" (
    echo   [OK]   Secure Boot is disabled
) else (
    echo   [INFO] Secure Boot status: %SB% ^(probably legacy BIOS - OK^)
)
echo.

REM ============================================================
REM 3. VulnerableDriverBlocklist
REM ============================================================
echo [3/8] VulnerableDriverBlocklist...
for /f "tokens=3" %%i in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\CI\Config" /v VulnerableDriverBlocklistEnable 2^>nul ^| findstr "VulnerableDriverBlocklistEnable"') do set VDB=%%i
if "%VDB%"=="0x0" (
    echo   [OK]   Blocklist is disabled
) else (
    if "%VDB%"=="" (
        echo   [INFO] VulnerableDriverBlocklistEnable not set ^(default = enabled^)
    ) else (
        echo   [FAIL] Blocklist is ENABLED ^(value=%VDB%^) - kdmapper will fail
    )
    choice /c YN /n /m "         Disable it now? [Y/N]: "
    if errorlevel 2 (
        echo          Skipped. Without fix kdmap may return STATUS_IMAGE_CERT_REVOKED.
        set ANY_PROBLEM=1
    ) else (
        reg add "HKLM\SYSTEM\CurrentControlSet\Control\CI\Config" /v VulnerableDriverBlocklistEnable /t REG_DWORD /d 0 /f >nul
        echo   [FIX]  Set to 0. REBOOT REQUIRED to take effect.
        set NEED_REBOOT=1
    )
)
echo.

REM ============================================================
REM 4. HVCI / Memory Integrity
REM ============================================================
echo [4/8] HVCI ^(Memory Integrity^)...
for /f "tokens=3" %%i in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" /v Enabled 2^>nul ^| findstr /i "Enabled"') do set HVCI=%%i
if "%HVCI%"=="0x0" (
    echo   [OK]   HVCI is disabled
) else if "%HVCI%"=="" (
    echo   [OK]   HVCI key not set ^(default = off^)
) else (
    echo   [FAIL] HVCI is ENABLED ^(value=%HVCI%^) - unsigned drivers will fail
    choice /c YN /n /m "         Disable it now? [Y/N]: "
    if errorlevel 2 (
        echo          Skipped.
        set ANY_PROBLEM=1
    ) else (
        reg add "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" /v Enabled /t REG_DWORD /d 0 /f >nul
        echo   [FIX]  Set to 0. REBOOT REQUIRED.
        set NEED_REBOOT=1
    )
)
echo.

REM ============================================================
REM 5. Windows Defender Real-Time Protection
REM ============================================================
echo [5/8] Defender Real-Time Protection...
for /f "tokens=*" %%i in ('powershell -NoProfile -Command "try { (Get-MpPreference).DisableRealtimeMonitoring } catch { 'N/A' }" 2^>nul') do set DEF=%%i
if /i "%DEF%"=="True" (
    echo   [OK]   Real-Time Monitoring is DISABLED
) else if /i "%DEF%"=="False" (
    echo   [INFO] Real-Time Monitoring is ON ^(may cause 0xC0000022^)
    choice /c YN /n /m "         Temporarily disable it? [Y/N]: "
    if errorlevel 2 (
        echo          Skipped. If you hit 0xC0000022, retry with --indPages or disable here.
    ) else (
        powershell -NoProfile -Command "Set-MpPreference -DisableRealtimeMonitoring $true" 2>nul
        if errorlevel 1 (
            echo   [FAIL] Could not disable ^(tamper protection?^). Disable manually:
            echo          Windows Security -^> Virus -^> Manage settings -^> Real-time protection OFF
        ) else (
            echo   [FIX]  Disabled until next reboot.
        )
    )
) else (
    echo   [INFO] Defender status: %DEF%
)
echo.

REM ============================================================
REM 6. Detect running AntiCheat processes
REM ============================================================
echo [6/8] Running AntiCheat processes...
set AC_FOUND=0
for %%P in (vgc.exe vgtray.exe faceitclient.exe faceitservice.exe ESEAClient.exe EasyAntiCheat.exe BEService.exe BEServiceCS2.exe ricochet.exe) do (
    tasklist /fi "imagename eq %%P" 2>nul | findstr /i "%%P" >nul && (
        echo   [WARN] %%P is running - may block kdmapper
        set AC_FOUND=1
    )
)
if "%AC_FOUND%"=="0" (
    echo   [OK]   No common AC processes detected
) else (
    choice /c YN /n /m "         Try to terminate them now? [Y/N]: "
    if errorlevel 2 (
        echo          Skipped. Close them yourself before START.bat.
    ) else (
        for %%P in (vgc.exe vgtray.exe faceitclient.exe faceitservice.exe ESEAClient.exe EasyAntiCheat.exe BEService.exe BEServiceCS2.exe) do (
            taskkill /f /im %%P >nul 2>&1
        )
        echo   [FIX]  Sent taskkill to known AC processes
    )
)
echo.

REM ============================================================
REM 7. Detect lingering iqvw64e (causes "\Device\Nal already in use")
REM ============================================================
echo [7/8] Lingering Intel driver ^(iqvw64e^)...
sc query iqvw64e 2>nul | findstr /i "STATE" >nul && (
    echo   [WARN] iqvw64e service exists - may cause "Device\Nal in use" on next map
    choice /c YN /n /m "         Stop ^& delete it? [Y/N]: "
    if errorlevel 2 (
        echo          Skipped.
    ) else (
        sc stop iqvw64e >nul 2>&1
        sc delete iqvw64e >nul 2>&1
        echo   [FIX]  iqvw64e service removed
    )
) || (
    echo   [OK]   No iqvw64e service installed
)
echo.

REM ============================================================
REM 8. Detect tracked F20Driver / IsValveDS_Driver mappings
REM ============================================================
echo [8/8] Existing kdmap_tracker records...
reg query "HKLM\SOFTWARE\kdmap_tracker" 2>nul | findstr "F20Driver" >nul && (
    echo   [INFO] F20Driver mapping is tracked - kdunmap.exe can free it
) || (
    echo   [OK]   No F20Driver mapping tracked
)
reg query "HKLM\SOFTWARE\kdmap_tracker" 2>nul | findstr "IsValveDS_Driver" >nul && (
    echo   [INFO] IsValveDS_Driver mapping is tracked - kdunmap.exe can free it
) || (
    echo   [OK]   No IsValveDS_Driver mapping tracked
)
echo.

echo ====================================================
if "%ANY_PROBLEM%"=="1" (
    if "%NEED_REBOOT%"=="1" (
        echo  ACTION REQUIRED: REBOOT to apply registry fixes,
        echo  then re-run preflight.bat to verify.
    ) else (
        echo  Some checks failed - see [FAIL] above and fix manually.
    )
) else (
    if "%NEED_REBOOT%"=="1" (
        echo  ALL CHECKS OK ^(after reboot for applied fixes^).
        echo  REBOOT NOW, then run START.bat.
    ) else (
        echo  ALL CHECKS PASSED. You can run START.bat.
    )
)
echo ====================================================
echo.
pause
