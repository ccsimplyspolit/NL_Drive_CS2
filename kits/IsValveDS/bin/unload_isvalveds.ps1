$ErrorActionPreference = 'Stop'

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
    public const uint SYNCHRONIZE        = 0x00100000;
    public const uint WAIT_OBJECT_0      = 0x00000000;
}
'@
Add-Type -TypeDefinition $src -Language CSharp | Out-Null

$name = "Global\IsValveDSStop"
$doneName = "Global\IsValveDSStopped"
$doneAccess = [N]::SYNCHRONIZE -bor [N]::EVENT_MODIFY_STATE
$doneHandle = [N]::OpenEvent($doneAccess, $false, $doneName)
if ($doneHandle -ne [IntPtr]::Zero) {
    Write-Host "Done event found; will wait for explicit worker-exit signal." -ForegroundColor DarkGray
    [void][N]::ResetEvent($doneHandle)
}

$h = [N]::OpenEvent([N]::EVENT_MODIFY_STATE, $false, $name)
if ($h -eq [IntPtr]::Zero) {
    Write-Host "IsValveDS_Driver not loaded (event $name not found)" -ForegroundColor Yellow
    if ($doneHandle -ne [IntPtr]::Zero) { [void][N]::CloseHandle($doneHandle) }
    exit 2
}
[void][N]::SetEvent($h)
[void][N]::CloseHandle($h)
Write-Host "Stop signal sent. Worker thread will release SHM + event handles." -ForegroundColor Green

$maxWaitMs = 10000
$elapsed = 0
$cleanedUp = $false

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
        Start-Sleep -Milliseconds 100
        $elapsed += 100
        $h2 = [N]::OpenEvent([N]::SYNCHRONIZE, $false, $name)
        if ($h2 -eq [IntPtr]::Zero) {
            $cleanedUp = $true
            Write-Host ("Driver released stop-event handle after {0} ms (legacy clean exit)." -f $elapsed) -ForegroundColor Green
            break
        }
        [void][N]::CloseHandle($h2)
    }
}

if (-not $cleanedUp) {
    Write-Host "Driver did not confirm clean worker exit; reboot is required before remapping." -ForegroundColor Red
    exit 3
}

Start-Sleep -Milliseconds 1500
exit 0
