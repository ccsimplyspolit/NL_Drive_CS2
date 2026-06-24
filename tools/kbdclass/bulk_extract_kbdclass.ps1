# bulk_extract_kbdclass.ps1
# -----------------------------------------------------------------------------
# Dev-time tool. Generates known-good (sha256 -> RVA) pairs for kbdclass.sys
# across many Windows builds by:
#
#   1. Fetching WinBindex index (https://winbindex.m417z.com/) for kbdclass.sys
#      - This is a public open-source index of every Windows binary by hash +
#        build. Updated regularly. ~25 KB compressed JSON.
#
#   2. For each unique kbdclass.sys hash:
#      - Downloads the binary from Microsoft Symbol Server using
#        timestamp + virtualSize from the index entry.
#      - Cached locally so re-runs don't re-download.
#
#   3. Runs analyze_kbdclass.exe --dry on each downloaded file:
#      - Returns the matched signature + RVA, or "no match".
#
#   4. Prints results as C++ KBDCLASS_KNOWN[] entries ready to paste into
#      F20Driver/main.cpp.
#
# Re-run this whenever:
#   - A new Windows build ships (new entries appear in WinBindex)
#   - We add new signatures to analyze_kbdclass.cpp (might match previous "miss")
# -----------------------------------------------------------------------------

param(
    [string]$Analyzer = "$PSScriptRoot\..\..\build\bin\analyze_kbdclass.exe",
    [string]$CacheDir = "$PSScriptRoot\cache_kbdclass"
)

$winbindexUrl = "https://winbindex.m417z.com/data/by_filename_compressed/kbdclass.sys.json.gz"
$msSymbolBase = "https://msdl.microsoft.com/download/symbols"

if (-not (Test-Path $Analyzer)) {
    Write-Error "Analyzer not found: $Analyzer"
    Write-Error "Build it first: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_release.ps1 -SkipPackage"
    exit 1
}

New-Item -ItemType Directory -Path $CacheDir -Force | Out-Null

Write-Host "Fetching WinBindex index..."
$gzPath = "$CacheDir\index.json.gz"
Invoke-WebRequest -Uri $winbindexUrl -UseBasicParsing -OutFile $gzPath -TimeoutSec 30

$stream = [System.IO.File]::OpenRead($gzPath)
$gzip = New-Object System.IO.Compression.GzipStream($stream, [System.IO.Compression.CompressionMode]::Decompress)
$reader = New-Object System.IO.StreamReader($gzip)
$json = $reader.ReadToEnd()
$reader.Close(); $gzip.Close(); $stream.Close()

$index = $json | ConvertFrom-Json
$entries = @($index.PSObject.Properties)
Write-Host ("WinBindex has " + $entries.Count + " unique kbdclass.sys hashes")

$results = @()
$i = 0
foreach ($prop in $entries) {
    $i++
    $sha = $prop.Name
    $info = $prop.Value.fileInfo
    if (-not $info.timestamp -or -not $info.virtualSize) {
        Write-Host ("  [SKIP $i/" + $entries.Count + "] $($sha.Substring(0,16))... no ts/virtualSize")
        continue
    }
    $ts = "{0:X}" -f $info.timestamp
    $vs = "{0:X}" -f $info.virtualSize
    $cache = "$CacheDir\$sha.sys"
    $url = "$msSymbolBase/kbdclass.sys/$ts$vs/kbdclass.sys"
    if (-not (Test-Path $cache)) {
        try {
            Invoke-WebRequest -Uri $url -UseBasicParsing -OutFile $cache -TimeoutSec 20 -ErrorAction Stop
        } catch {
            Write-Host ("  [FAIL $i/" + $entries.Count + "] $($sha.Substring(0,16))... download 404")
            continue
        }
    }
    $verSet = @($prop.Value.windowsVersions.PSObject.Properties.Name)
    $verLabel = if ($verSet.Count -gt 0) { $verSet[0] } else { "?" }
    $output = & $Analyzer --dry $cache 2>&1
    $found = $output | Where-Object { $_ -match "Found via" }
    if ($found) {
        $found -match "Found via (\S+):.*RVA=0x([0-9A-Fa-f]+)" | Out-Null
        $sigName = $matches[1]
        $rva = $matches[2]
        $results += [PSCustomObject]@{
            Sha256  = $sha.ToUpper()
            RVA     = $rva
            Sig     = $sigName
            Version = $info.version
            WinVer  = $verLabel
        }
        Write-Host ("  [OK   $i/" + $entries.Count + "] $($sha.Substring(0,16))... RVA=0x$rva ($sigName) [$verLabel $($info.version)]")
    } else {
        Write-Host ("  [MISS $i/" + $entries.Count + "] $($sha.Substring(0,16))... [$verLabel $($info.version)]")
    }
}

Write-Host ""
Write-Host ("=== Found RVAs for " + $results.Count + "/" + $entries.Count + " entries ===")
$results | Format-Table -AutoSize | Out-String -Width 200 | Write-Host

# Generate C++ paste-able snippet
Write-Host ""
Write-Host "=== Paste this into F20Driver/main.cpp g_KnownKbdclass[] ==="
Write-Host ""
foreach ($r in $results) {
    Write-Host ('    { "' + $r.Sha256 + '",')
    Write-Host ('      0x' + $r.RVA + ', "' + $r.WinVer + ' (' + $r.Version + ')" },')
}
Write-Host ""
Write-Host "Misses likely use older Win10 signatures (1511/1607/1709/1803/1809) or"
Write-Host "newer Win11 21H2 builds. Add new sigs to analyze_kbdclass.cpp and rerun."
