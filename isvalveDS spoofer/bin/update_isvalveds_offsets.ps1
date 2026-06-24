param(
    [string]$CacheDir = $null,
    [switch]$NoRegistry
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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
$regPath    = 'HKLM:\SOFTWARE\IsValveDS'
$userAgent  = 'IsValveDS-OffsetsFetcher/1.0 (+update_isvalveds_offsets.ps1)'

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

    $dwRaw = Get-JsonPath -Root $offsets -Path @('client.dll','dwGameRules')
    if ($null -eq $dwRaw) {
        throw 'JSON schema changed: client.dll.dwGameRules not found in offsets.json'
    }
    $dwGameRules = [uint64]$dwRaw

    $mRaw = Get-JsonPath -Root $client -Path @('client.dll','classes','C_CSGameRules','fields','m_bIsValveDS')
    if ($null -eq $mRaw) {
        $mRaw = Get-JsonPath -Root $client -Path @('client.dll','classes','CCSGameRules','fields','m_bIsValveDS')
    }
    if ($null -eq $mRaw) {
        throw 'JSON schema changed: C_CSGameRules/CCSGameRules.m_bIsValveDS not found in client_dll.json'
    }
    $mIsValveDS = [uint32]$mRaw

    if ($dwGameRules -lt 0x100000 -or $dwGameRules -gt 0x40000000 -or (($dwGameRules -band 7) -ne 0)) {
        throw ("dwGameRules sanity check failed: 0x{0:X}" -f $dwGameRules)
    }
    if ($mIsValveDS -eq 0 -or $mIsValveDS -gt 0x10000) {
        throw ("m_bIsValveDS sanity check failed: 0x{0:X}" -f $mIsValveDS)
    }

    Write-Host ("dwGameRules = 0x{0:X}" -f $dwGameRules)
    Write-Host ("m_bIsValveDS = 0x{0:X}" -f $mIsValveDS)

    if ($NoRegistry) {
        Write-Host 'GitHub data downloaded and validated. NoRegistry set; registry write skipped.'
        exit 0
    }

    Write-Host 'GitHub data downloaded and validated; replacing registry offsets now.'

    New-Item -Path $regPath -Force | Out-Null
    New-ItemProperty -Path $regPath -Name 'Cs2DwGameRules' -PropertyType QWord  -Value $dwGameRules -Force | Out-Null
    New-ItemProperty -Path $regPath -Name 'Cs2M_bIsValveDS' -PropertyType DWord -Value $mIsValveDS  -Force | Out-Null
    New-ItemProperty -Path $regPath -Name 'Cs2OffsetsSource' -PropertyType String -Value 'github:a2x/cs2-dumper/main' -Force | Out-Null
    New-ItemProperty -Path $regPath -Name 'Cs2OffsetsFetchedAtUtc' -PropertyType String -Value ([DateTime]::UtcNow.ToString('o')) -Force | Out-Null

    Write-Host 'Wrote CS2 offsets to HKLM\SOFTWARE\IsValveDS'
    exit 0
} catch {
    Write-Host "Failed to update IsValveDS offsets from GitHub: $($_.Exception.Message)"
    exit 2
}
