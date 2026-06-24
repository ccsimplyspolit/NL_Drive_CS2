@echo off
setlocal EnableDelayedExpansion
title F20Kit STOP
cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

echo ====================================================
echo                   F20Kit STOP
echo ====================================================
echo.

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss" 2^>nul') do set STOPDT=%%i
if not defined STOPDT set STOPDT=unknown_time
set STOPLOGROOT=%~dp0logs
set STOPDIR=%STOPLOGROOT%\stop_%STOPDT%
mkdir "%STOPLOGROOT%" 2>nul
mkdir "%STOPDIR%" 2>nul

(
    echo F20Kit stop diagnostics
    echo Generated=%DATE% %TIME%
    echo Folder=%STOPDIR%
    echo Working dir=%CD%
    echo.
    echo === F20Driver tracker before stop ===
    reg query "HKLM\SOFTWARE\kdmap_tracker\F20Driver"
    echo.
    echo === F20Driver registry ===
    reg query "HKLM\SOFTWARE\F20Driver"
    echo.
    echo === File hashes ===
    powershell -NoProfile -Command "Get-FileHash '%~dp0F20Driver.sys','%~dp0kdunmap.exe','%~dp0unload_f20.ps1','%~dp0cleanup_f20_state.ps1','%~dp0update_cs2_offsets.ps1' -Algorithm SHA256 | Format-List"
) > "%STOPDIR%\summary.txt" 2>&1

echo [1/2] Signaling F20Driver stop event and waiting for worker exit...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0unload_f20.ps1" > "%STOPDIR%\soft_unload.txt" 2>&1
set SOFTRC=!errorlevel!
type "%STOPDIR%\soft_unload.txt"
echo.

if "!SOFTRC!"=="2" (
    echo [i] No live F20Driver stop event found.
    reg query "HKLM\SOFTWARE\kdmap_tracker\F20Driver" >nul 2>&1 && (
        reg delete "HKLM\SOFTWARE\kdmap_tracker\F20Driver" /f > "%STOPDIR%\stale_tracker_delete.txt" 2>&1
        echo [i] Removed stale kdmap tracker. No blind free was attempted.
    )
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cleanup_f20_state.ps1" -ClearTracker > "%STOPDIR%\state_cleanup.txt" 2>&1
    echo.
    echo Logs saved in:
    echo   %STOPDIR%
    pause
    exit /b 0
)

if "!SOFTRC!"=="3" (
    echo [!] Driver did not confirm clean worker exit.
    echo     NOT running kdunmap, because freeing live code can BSOD.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cleanup_f20_state.ps1" > "%STOPDIR%\state_cleanup.txt" 2>&1
    goto :reboot_prompt
)

if not "!SOFTRC!"=="0" (
    echo [!] Soft unload failed with rc=!SOFTRC!.
    echo     NOT running kdunmap, because driver state is unknown.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cleanup_f20_state.ps1" > "%STOPDIR%\state_cleanup.txt" 2>&1
    goto :reboot_prompt
)

echo [2/2] Worker exit confirmed. Freeing tracked kernel allocation...
"%~dp0kdunmap.exe" --key F20Driver --alreadyStopped > "%STOPDIR%\kdunmap.txt" 2>&1
set KDUNMAPRC=!errorlevel!
type "%STOPDIR%\kdunmap.txt"
echo.

if "!KDUNMAPRC!"=="0" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cleanup_f20_state.ps1" -ClearTracker > "%STOPDIR%\state_cleanup.txt" 2>&1
    echo ====================================================
    echo  TRUE UNLOAD COMPLETE - kernel allocation freed.
    echo  Logs:
    echo    %STOPDIR%
    echo ====================================================
    echo.
    pause
    exit /b 0
)

if "!KDUNMAPRC!"=="2" (
    echo [i] No kdmap tracking record found after clean worker exit.
    echo     Driver is stopped; code may remain in kernel until reboot.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cleanup_f20_state.ps1" -ClearTracker > "%STOPDIR%\state_cleanup.txt" 2>&1
    goto :reboot_prompt
)

echo [!] kdunmap failed with rc=!KDUNMAPRC!.
echo     Driver worker is stopped, but code may remain in kernel until reboot.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cleanup_f20_state.ps1" > "%STOPDIR%\state_cleanup.txt" 2>&1

:reboot_prompt
echo.
echo Logs saved in:
echo   %STOPDIR%
echo.
echo A reboot is the only safe full cleanup path now.
echo.
choice /c YN /n /m "Reboot now? (force-close all apps) [Y/N]: "
if errorlevel 2 goto :no_reboot

echo.
echo [!] System will restart in 5 seconds with /f.
echo     Press Ctrl+C now to abort.
echo.
timeout /t 5
shutdown /r /f /t 0 /c "F20Driver safe cleanup"
exit /b 0

:no_reboot
echo.
echo Skipping reboot. Do NOT run START.bat again until after reboot.
echo.
pause
exit /b 1
