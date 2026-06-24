param(
    [switch]$ClearTracker,
    [switch]$ClearCs2Offsets
)

$ErrorActionPreference = 'Continue'

function Remove-RegTree {
    param([string]$Path, [string]$Name)

    if (Test-Path $Path) {
        try {
            Remove-Item -Path $Path -Recurse -Force -ErrorAction Stop
            Write-Host "Removed ${Name}: $Path"
        } catch {
            Write-Host "Failed to remove ${Name}: $Path :: $($_.Exception.Message)"
        }
    } else {
        Write-Host "${Name} not present: $Path"
    }
}

function Stop-DeleteService {
    param([string]$Name)

    $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if ($svc) {
        try {
            if ($svc.Status -ne 'Stopped') {
                Stop-Service -Name $Name -Force -ErrorAction SilentlyContinue
                Start-Sleep -Milliseconds 300
            }
        } catch {}
        try {
            & sc.exe delete $Name | Out-Host
        } catch {
            Write-Host "Failed to delete service $Name :: $($_.Exception.Message)"
        }
    } else {
        & sc.exe query $Name *> $null
        if ($LASTEXITCODE -eq 0) {
            try { & sc.exe delete $Name | Out-Host } catch {}
        } else {
            Write-Host "Service not present: $Name"
        }
    }
}

function Remove-RegValues {
    param([string]$Path, [string[]]$Names, [string]$Name)

    if (-not (Test-Path $Path)) {
        Write-Host "${Name} key not present: $Path"
        return
    }

    foreach ($valueName in $Names) {
        try {
            Remove-ItemProperty -Path $Path -Name $valueName -Force -ErrorAction Stop
            Write-Host "Removed ${Name} value: $valueName"
        } catch {
            Write-Host "${Name} value not present: $valueName"
        }
    }
}

$f20RegPath = 'HKLM:\SOFTWARE\F20Driver'

Remove-RegValues -Path $f20RegPath -Name 'analyzer registry' -Names @(
    'CallbackRva',
    'KbdTimestamp',
    'KbdImageSize',
    'KbdSha256',
    'Signature'
)

if ($ClearCs2Offsets) {
    Remove-RegValues -Path $f20RegPath -Name 'CS2 offsets' -Names @(
        'Cs2DwLocalPlayerController',
        'Cs2M_pActionTrackingServices',
        'Cs2M_iNumRoundKills',
        'Cs2OffsetsSource',
        'Cs2OffsetsFetchedAtUtc'
    )
}

if ($ClearTracker) {
    Remove-RegTree -Path 'HKLM:\SOFTWARE\kdmap_tracker\F20Driver' -Name 'kdmap tracker'
}

Stop-DeleteService -Name 'F20Driver'
Stop-DeleteService -Name 'iqvw64e'

exit 0
