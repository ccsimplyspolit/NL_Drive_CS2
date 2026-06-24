param(
    [string]$CacheDir = $null,
    [switch]$NoRegistry
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# GitHub raw refuses TLS 1.0 and (since 2022) often refuses TLS 1.1. On stock
# Windows PowerShell 5.1 the default protocol list does NOT include TLS 1.2 on
# older builds, which manifests as opaque "Could not create SSL/TLS secure
# channel" errors. Enforce 1.2 + 1.3.
try {
    $protos = [Net.SecurityProtocolType]::Tls12
    if ([Enum]::GetNames([Net.SecurityProtocolType]) -contains 'Tls13') {
        $protos = $protos -bor [Net.SecurityProtocolType]::Tls13
    }
    [Net.ServicePointManager]::SecurityProtocol = $protos
} catch {
    Write-Host "Warning: could not force TLS 1.2/1.3 protocol: $($_.Exception.Message)"
}

$offsetsUrl = 'https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/offsets.json'
$clientUrl  = 'https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/client_dll.json'
$regPath    = 'HKLM:\SOFTWARE\F20Driver'
$userAgent  = 'F20Kit-OffsetsFetcher/1.0 (+update_cs2_offsets.ps1)'

function Download-Json {
    param([string]$Url, [string]$Name)

    Write-Host "Downloading $Name from $Url"
    $text = (Invoke-WebRequest -UseBasicParsing -Uri $Url -TimeoutSec 20 -UserAgent $userAgent).Content
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "$Name download returned empty content"
    }

    if ($CacheDir) {
        New-Item -ItemType Directory -Path $CacheDir -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $CacheDir $Name) -Value $text -Encoding UTF8
    }

    return ($text | ConvertFrom-Json)
}

# Safely navigate a nested JSON object by a list of property names.
# Returns $null if any segment is missing. Replaces direct member-access that
# would silently return $null under StrictMode/PS, then crash with a
# misleading "cannot convert to uint" later in the pipeline.
function Get-JsonPath {
    param(
        [Parameter(Mandatory)] $Root,
        [Parameter(Mandatory)] [string[]] $Path
    )
    $cur = $Root
    foreach ($seg in $Path) {
        if ($null -eq $cur) { return $null }
        $member = $cur.PSObject.Properties[$seg]
        if ($null -eq $member) { return $null }
        $cur = $member.Value
    }
    return $cur
}

try {
    $offsets = Download-Json -Url $offsetsUrl -Name 'cs2_offsets.json'
    $client  = Download-Json -Url $clientUrl  -Name 'cs2_client_dll.json'

    $dwLocalRaw = Get-JsonPath -Root $offsets -Path @('client.dll','dwLocalPlayerController')
    $mActRaw    = Get-JsonPath -Root $client  -Path @('client.dll','classes','CCSPlayerController','fields','m_pActionTrackingServices')
    $mKillsRaw  = Get-JsonPath -Root $client  -Path @('client.dll','classes','CCSPlayerController_ActionTrackingServices','fields','m_iNumRoundKills')

    if ($null -eq $dwLocalRaw) { throw 'JSON schema changed: client.dll.dwLocalPlayerController not found in offsets.json' }
    if ($null -eq $mActRaw)    { throw 'JSON schema changed: CCSPlayerController.m_pActionTrackingServices not found in client_dll.json' }
    if ($null -eq $mKillsRaw)  { throw 'JSON schema changed: CCSPlayerController_ActionTrackingServices.m_iNumRoundKills not found in client_dll.json' }

    $dwLocalPlayerController = [uint64]$dwLocalRaw
    $mActionTracking         = [uint32]$mActRaw
    $mNumRoundKills          = [uint32]$mKillsRaw

    if ($dwLocalPlayerController -lt 0x100000 -or $dwLocalPlayerController -gt 0x10000000) {
        throw ("dwLocalPlayerController sanity check failed: 0x{0:X}" -f $dwLocalPlayerController)
    }
    if ($mActionTracking -eq 0 -or $mActionTracking -gt 0x100000) {
        throw ("m_pActionTrackingServices sanity check failed: 0x{0:X}" -f $mActionTracking)
    }
    if ($mNumRoundKills -eq 0 -or $mNumRoundKills -gt 0x10000) {
        throw ("m_iNumRoundKills sanity check failed: 0x{0:X}" -f $mNumRoundKills)
    }

    Write-Host ("dwLocalPlayerController = 0x{0:X}" -f $dwLocalPlayerController)
    Write-Host ("m_pActionTrackingServices = 0x{0:X}" -f $mActionTracking)
    Write-Host ("m_iNumRoundKills = 0x{0:X}" -f $mNumRoundKills)

    if ($NoRegistry) {
        Write-Host "GitHub data downloaded and validated. NoRegistry set; registry write skipped."
        exit 0
    }

    Write-Host "GitHub data downloaded and validated; replacing registry offsets now."

    New-Item -Path $regPath -Force | Out-Null
    New-ItemProperty -Path $regPath -Name 'Cs2DwLocalPlayerController' -PropertyType QWord  -Value $dwLocalPlayerController -Force | Out-Null
    New-ItemProperty -Path $regPath -Name 'Cs2M_pActionTrackingServices' -PropertyType DWord -Value $mActionTracking -Force | Out-Null
    New-ItemProperty -Path $regPath -Name 'Cs2M_iNumRoundKills' -PropertyType DWord -Value $mNumRoundKills -Force | Out-Null
    New-ItemProperty -Path $regPath -Name 'Cs2OffsetsSource' -PropertyType String -Value 'github:a2x/cs2-dumper/main' -Force | Out-Null
    New-ItemProperty -Path $regPath -Name 'Cs2OffsetsFetchedAtUtc' -PropertyType String -Value ([DateTime]::UtcNow.ToString('o')) -Force | Out-Null

    Write-Host "Wrote CS2 offsets to HKLM\SOFTWARE\F20Driver"
    exit 0
} catch {
    Write-Host "Failed to update CS2 offsets from GitHub: $($_.Exception.Message)"
    exit 2
}
