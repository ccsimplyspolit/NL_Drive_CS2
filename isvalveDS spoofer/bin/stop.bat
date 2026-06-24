@echo off
setlocal EnableDelayedExpansion
title IsValveDS Spoofer STOP
cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath $env:ComSpec -ArgumentList '/k \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss" 2^>nul') do set STOPDT=%%i
if not defined STOPDT set STOPDT=unknown_time
set STOPLOGROOT=%~dp0logs
set STOPDIR=%STOPLOGROOT%\stop_%STOPDT%
mkdir "%STOPLOGROOT%" 2>nul
mkdir "%STOPDIR%" 2>nul

(
    echo IsValveDS stop diagnostics
    echo Generated=%DATE% %TIME%
    echo Folder=%STOPDIR%
    echo Working dir=%CD%
    echo.
    echo === IsValveDS tracker before stop ===
    reg query "HKLM\SOFTWARE\kdmap_tracker\IsValveDS_Driver"
    echo.
    echo === IsValveDS offsets registry ===
    reg query "HKLM\SOFTWARE\IsValveDS"
    echo.
    echo === File hashes ===
    powershell -NoProfile -Command "Get-FileHash '%~dp0IsValveDS_Driver.sys','%~dp0kdunmap.exe','%~dp0unload_isvalveds.ps1','%~dp0update_isvalveds_offsets.ps1' -Algorithm SHA256 | Format-List"
) > "%STOPDIR%\summary.txt" 2>&1

echo ====================================================
echo            IsValveDS Spoofer  -  STOP
echo ====================================================
echo.

echo [*] Stopping IsValveDS_Console.exe (if running)...
taskkill /f /im IsValveDS_Console.exe >nul 2>&1

echo [1/2] Signaling IsValveDS_Driver stop event and waiting for worker exit...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0unload_isvalveds.ps1" > "%STOPDIR%\soft_unload.txt" 2>&1
set SOFTRC=!errorlevel!
type "%STOPDIR%\soft_unload.txt"
echo.

if "!SOFTRC!"=="2" (
    echo [i] No live IsValveDS_Driver stop event found.
    reg query "HKLM\SOFTWARE\kdmap_tracker\IsValveDS_Driver" >nul 2>&1 && (
        reg delete "HKLM\SOFTWARE\kdmap_tracker\IsValveDS_Driver" /f > "%STOPDIR%\stale_tracker_delete.txt" 2>&1
        echo [i] Removed stale kdmap tracker. No blind free was attempted.
    )
    sc stop iqvw64e >nul 2>&1
    sc delete iqvw64e >nul 2>&1
    echo Logs saved in:
    echo   %STOPDIR%
    pause
    exit /b 0
)

if "!SOFTRC!"=="3" (
    echo [!] Driver did not confirm clean worker exit.
    echo     NOT running kdunmap, because freeing live code can BSOD.
    goto :reboot_prompt
)

if not "!SOFTRC!"=="0" (
    echo [!] Soft unload failed with rc=!SOFTRC!.
    echo     NOT running kdunmap, because driver state is unknown.
    goto :reboot_prompt
)

echo [2/2] Worker exit confirmed. Freeing tracked kernel allocation...
"%~dp0kdunmap.exe" --key IsValveDS_Driver --alreadyStopped > "%STOPDIR%\kdunmap.txt" 2>&1
set KDUNMAPRC=!errorlevel!
type "%STOPDIR%\kdunmap.txt"
echo.

if "!KDUNMAPRC!"=="0" (
    sc stop iqvw64e >nul 2>&1
    sc delete iqvw64e >nul 2>&1
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
    goto :reboot_prompt
)

echo [!] kdunmap failed with rc=!KDUNMAPRC!.
echo     Driver worker is stopped, but code may remain in kernel until reboot.

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
shutdown /r /f /t 0 /c "IsValveDS safe cleanup"
exit /b 0

:no_reboot
echo.
echo Skipping reboot. Do NOT run run.bat again until after reboot.
echo.
pause
exit /b 1
