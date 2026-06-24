Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Add-Type compiles via csc.exe and costs ~1s on first call. If this script is
# invoked repeatedly inside the same PowerShell session (or from START.bat
# which already loaded it), the type is cached -- skip recompilation.
if (-not ('N' -as [type])) {
    $src = @'
using System;
using System.Runtime.InteropServices;
public static class N {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Auto)]
    public static extern IntPtr OpenEvent(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool SetEvent(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ResetEvent(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern uint WaitForSingleObject(IntPtr h, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool CloseHandle(IntPtr h);
    public const uint EVENT_MODIFY_STATE = 0x0002;
    public const uint EVENT_QUERY_STATE  = 0x0001;
    public const uint SYNCHRONIZE        = 0x00100000;
    public const uint WAIT_OBJECT_0      = 0x00000000;
    public const uint WAIT_TIMEOUT       = 0x00000102;
}
'@
    Add-Type -TypeDefinition $src -Language CSharp | Out-Null
}

$name = "Global\F20DriverStop"
$doneName = "Global\F20DriverStopped"
$doneAccess = [N]::SYNCHRONIZE -bor [N]::EVENT_MODIFY_STATE
$doneHandle = [N]::OpenEvent($doneAccess, $false, $doneName)
if ($doneHandle -ne [IntPtr]::Zero) {
    Write-Host "Done event found; will wait for explicit worker-exit signal." -ForegroundColor DarkGray
    [void][N]::ResetEvent($doneHandle)
}

# 1. Signal stop event.
$h = [N]::OpenEvent([N]::EVENT_MODIFY_STATE, $false, $name)
if ($h -eq [IntPtr]::Zero) {
    Write-Host "F20Driver not loaded (event $name not found)" -ForegroundColor Yellow
    if ($doneHandle -ne [IntPtr]::Zero) { [void][N]::CloseHandle($doneHandle) }
    # Exit code 2 = nothing to do (driver wasn't loaded). Distinguishes from
    # exit code 0 (was loaded, signaled) so START.bat / STOP.bat can act.
    exit 2
}
[void][N]::SetEvent($h)
[void][N]::CloseHandle($h)
Write-Host "Stop signal sent." -ForegroundColor Green

# 2. Wait for worker thread to actually consume the signal and tear down.
#    New driver builds signal Global\F20DriverStopped at the end of cleanup.
#    Older builds are handled by polling until the stop event disappears.
$maxWaitMs   = 10000
$pollMs      = 100
$elapsed     = 0
$cleanedUp   = $false

if ($doneHandle -ne [IntPtr]::Zero) {
    $wait = [N]::WaitForSingleObject($doneHandle, [uint32]$maxWaitMs)
    if ($wait -eq [N]::WAIT_OBJECT_0) {
        $cleanedUp = $true
        Write-Host "Driver signaled worker-exit done event." -ForegroundColor Green
    } else {
        Write-Host ("Done event wait timed out after {0} ms." -f $maxWaitMs) -ForegroundColor Yellow
    }
    [void][N]::CloseHandle($doneHandle)
}

if (-not $cleanedUp) {
    while ($elapsed -lt $maxWaitMs) {
        Start-Sleep -Milliseconds $pollMs
        $elapsed += $pollMs
        $h2 = [N]::OpenEvent([N]::SYNCHRONIZE, $false, $name)
        if ($h2 -eq [IntPtr]::Zero) {
            $cleanedUp = $true
            break
        }
        [void][N]::CloseHandle($h2)
    }
    if ($cleanedUp) {
        Write-Host ("Driver released its stop-event handle after {0} ms (legacy clean exit)." -f $elapsed) -ForegroundColor Green
    } else {
        Write-Host ("Stop event still present after {0} ms - worker might be busy or stuck." -f $maxWaitMs) -ForegroundColor Yellow
    }
}

# 3. Best-effort cleanup for ANY case where driver was somehow loaded as a
#    service (not the kdmapper path, but harmless if no such service exists).
$svc = Get-Service -Name "F20Driver" -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "Found service 'F20Driver' - stopping + deleting..." -ForegroundColor Yellow
    try { Stop-Service -Name "F20Driver" -Force -ErrorAction SilentlyContinue } catch {}
    try { & sc.exe delete F20Driver | Out-Null } catch {}
}

if (-not $cleanedUp) {
    Write-Host "Driver did not confirm clean worker exit; reboot is required before remapping." -ForegroundColor Red
    exit 3
}

# No final Start-Sleep here: the worker already gave its 50ms grace period in
# CleanupRuntimeResources before signalling F20DriverStopped, so by the time
# WAIT_OBJECT_0 returns above the thread is guaranteed to have exited.
Write-Host "(With kdmapper, driver code stays in kernel memory until reboot - this is expected.)" -ForegroundColor DarkGray
exit 0
