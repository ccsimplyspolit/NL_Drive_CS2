param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [switch]$SkipPackage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$msbuildCandidates = @(
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
)
$msbuild = $msbuildCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $msbuild) {
    throw 'MSBuild.exe not found. Install Visual Studio 2022 + WDK, or run from a Developer PowerShell.'
}

$projects = @(
    'src\drivers\F20Driver\F20Driver.vcxproj',
    'src\tools\analyze_kbdclass\analyze_kbdclass.vcxproj',
    'src\tools\kdmap\kdmap.vcxproj',
    'src\tools\kdunmap\kdunmap.vcxproj',
    'src\drivers\IsValveDS\IsValveDS_Driver.vcxproj',
    'src\apps\IsValveDSConsole\IsValveDS_Console.vcxproj'
)

foreach ($project in $projects) {
    Write-Host "=== BUILD $project ==="
    & $msbuild (Join-Path $repoRoot $project) /p:Configuration=$Configuration /p:Platform=$Platform /m
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed: $project"
    }
}

$binDir = Join-Path $repoRoot 'build\bin'
$f20Kit = Join-Path $repoRoot 'kits\F20Kit'
$isvKit = Join-Path $repoRoot 'kits\IsValveDS'
$isvBin = Join-Path $isvKit 'bin'

Copy-Item -LiteralPath (Join-Path $repoRoot 'src\drivers\F20Driver\x64\Release\F20Driver.sys') -Destination (Join-Path $f20Kit 'F20Driver.sys') -Force
Copy-Item -LiteralPath (Join-Path $binDir 'analyze_kbdclass.exe') -Destination (Join-Path $f20Kit 'analyze_kbdclass.exe') -Force
Copy-Item -LiteralPath (Join-Path $binDir 'kdmap.exe') -Destination (Join-Path $f20Kit 'kdmap.exe') -Force
Copy-Item -LiteralPath (Join-Path $binDir 'kdunmap.exe') -Destination (Join-Path $f20Kit 'kdunmap.exe') -Force

Copy-Item -LiteralPath (Join-Path $repoRoot 'src\drivers\IsValveDS\x64\Release\IsValveDS_Driver.sys') -Destination (Join-Path $isvBin 'IsValveDS_Driver.sys') -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'src\drivers\IsValveDS\x64\Release\IsValveDS_Driver.pdb') -Destination (Join-Path $isvBin 'IsValveDS_Driver.pdb') -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'src\apps\IsValveDSConsole\x64\Release\IsValveDS_Console.exe') -Destination (Join-Path $isvBin 'IsValveDS_Console.exe') -Force
Copy-Item -LiteralPath (Join-Path $binDir 'kdmap.exe') -Destination (Join-Path $isvBin 'kdmap.exe') -Force
Copy-Item -LiteralPath (Join-Path $binDir 'kdunmap.exe') -Destination (Join-Path $isvBin 'kdunmap.exe') -Force

if ($SkipPackage) {
    Write-Host 'Build complete; packaging skipped.'
    exit 0
}

$f20Zip = Join-Path $f20Kit 'F20Kit.zip'
$f20Files = Get-ChildItem -LiteralPath $f20Kit -File | Where-Object { $_.Name -notin @('F20Kit.zip','START_LAST.log') }
Compress-Archive -LiteralPath $f20Files.FullName -DestinationPath $f20Zip -Force

$stageRoot = Join-Path $repoRoot 'build\package\IsValveDS'
if (Test-Path -LiteralPath $stageRoot) {
    $resolvedStage = (Resolve-Path -LiteralPath $stageRoot).Path
    if (-not $resolvedStage.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to delete unexpected staging path: $resolvedStage"
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Path (Join-Path $stageRoot 'bin') | Out-Null
Copy-Item -LiteralPath (Join-Path $isvKit 'README.txt') -Destination $stageRoot
Copy-Item -LiteralPath (Join-Path $isvKit 'ИНСТРУКЦИЯ.txt') -Destination $stageRoot

$isvFiles = @(
    'IsValveDS_Driver.sys',
    'IsValveDS_Driver.pdb',
    'IsValveDS_Console.exe',
    'kdmap.exe',
    'kdunmap.exe',
    'preflight.bat',
    'run.bat',
    'stop.bat',
    'unload_isvalveds.ps1',
    'update_isvalveds_offsets.ps1'
)
foreach ($file in $isvFiles) {
    Copy-Item -LiteralPath (Join-Path $isvBin $file) -Destination (Join-Path $stageRoot 'bin')
}

$isvZip = Join-Path $isvKit 'IsValveDS_spoofer.zip'
Compress-Archive -Path (Join-Path $stageRoot '*') -DestinationPath $isvZip -Force

Get-FileHash $f20Zip,$isvZip -Algorithm SHA256 | Format-Table -AutoSize
