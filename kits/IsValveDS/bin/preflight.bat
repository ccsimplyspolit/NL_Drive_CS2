@echo off
chcp 65001 >nul 2>&1
title IsValveDS preflight
cd /d "%~dp0"

REM ----- elevate to admin -----
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

echo ====================================================
echo  PREFLIGHT - System checks for IsValveDS Spoofer
echo  Run me FIRST. I'll find blockers and offer fixes.
echo ====================================================
echo.

set ANY_PROBLEM=0
set NEED_REBOOT=0

REM ============================================================
REM 1. Secure Boot
REM ============================================================
echo [1/7] Secure Boot status...
for /f "tokens=*" %%i in ('powershell -NoProfile -Command "try { (Confirm-SecureBootUEFI) } catch { 'unsupported' }" 2^>nul') do set SB=%%i
if /i "%SB%"=="True" (
    echo   [FAIL] Secure Boot ENABLED - disable via BIOS ^(Security -^> Secure Boot OFF^)
    set ANY_PROBLEM=1
) else if /i "%SB%"=="False" (
    echo   [OK]   Secure Boot is disabled
) else (
    echo   [INFO] Secure Boot status: %SB% ^(probably legacy BIOS - OK^)
)
echo.

REM ============================================================
REM 2. VulnerableDriverBlocklist
REM ============================================================
echo [2/7] VulnerableDriverBlocklist...
for /f "tokens=3" %%i in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\CI\Config" /v VulnerableDriverBlocklistEnable 2^>nul ^| findstr "VulnerableDriverBlocklistEnable"') do set VDB=%%i
if "%VDB%"=="0x0" (
    echo   [OK]   Blocklist is disabled
) else (
    echo   [FAIL] Blocklist ENABLED ^(or default^) - will cause STATUS_IMAGE_CERT_REVOKED
    choice /c YN /n /m "         Disable it now? [Y/N]: "
    if errorlevel 2 (
        echo          Skipped.
        set ANY_PROBLEM=1
    ) else (
        reg add "HKLM\SYSTEM\CurrentControlSet\Control\CI\Config" /v VulnerableDriverBlocklistEnable /t REG_DWORD /d 0 /f >nul
        echo   [FIX]  Set to 0. REBOOT REQUIRED.
        set NEED_REBOOT=1
    )
)
echo.

REM ============================================================
REM 3. HVCI
REM ============================================================
echo [3/7] HVCI ^(Memory Integrity^)...
for /f "tokens=3" %%i in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" /v Enabled 2^>nul ^| findstr /i "Enabled"') do set HVCI=%%i
if "%HVCI%"=="0x0" (
    echo   [OK]   HVCI is disabled
) else if "%HVCI%"=="" (
    echo   [OK]   HVCI key not set ^(default = off^)
) else (
    echo   [FAIL] HVCI is ENABLED
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
REM 4. Anti-cheat / cs2.exe
REM ============================================================
echo [4/7] AntiCheat ^(IsValveDS needs cs2.exe running^)...
tasklist /fi "imagename eq cs2.exe" 2>nul | findstr /i "cs2.exe" >nul && (
    echo   [OK]   cs2.exe is running
) || (
    echo   [INFO] cs2.exe is NOT running - start the game before run.bat
)

set AC_FOUND=0
for %%P in (vgc.exe faceitclient.exe faceitservice.exe ESEAClient.exe EasyAntiCheat.exe BEService.exe) do (
    tasklist /fi "imagename eq %%P" 2>nul | findstr /i "%%P" >nul && (
        echo   [WARN] %%P is running
        set AC_FOUND=1
    )
)
if "%AC_FOUND%"=="1" (
    echo   [INFO] Detected AC processes can block kdmapper. Close them if you hit errors.
)
echo.

REM ============================================================
REM 5. Lingering iqvw64e
REM ============================================================
echo [5/7] Lingering Intel driver ^(iqvw64e^)...
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
REM 6. Tracked mapping
REM ============================================================
echo [6/7] Existing kdmap_tracker records...
reg query "HKLM\SOFTWARE\kdmap_tracker" 2>nul | findstr "IsValveDS_Driver" >nul && (
    echo   [INFO] IsValveDS_Driver mapping is tracked - kdunmap.exe can free it
) || (
    echo   [OK]   No IsValveDS_Driver mapping tracked
)
echo.

REM ============================================================
REM 7. Offsets updater
REM ============================================================
echo [7/7] CS2 offsets updater...
if exist "%~dp0update_isvalveds_offsets.ps1" (
    echo   [OK]   update_isvalveds_offsets.ps1 present
) else (
    echo   [FAIL] update_isvalveds_offsets.ps1 is missing
    set ANY_PROBLEM=1
)
reg query "HKLM\SOFTWARE\IsValveDS" 2>nul | findstr /i "Cs2DwGameRules Cs2M_bIsValveDS" >nul && (
    echo   [INFO] Existing registry offsets found
) || (
    echo   [INFO] No registry offsets yet - run.bat will download them before load
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
        echo  REBOOT NOW, then run run.bat.
    ) else (
        echo  ALL CHECKS PASSED. You can run run.bat.
    )
)
echo ====================================================
echo.
pause
