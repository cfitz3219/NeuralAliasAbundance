[CmdletBinding()]
param(
    [ValidateSet('Install','Rollback','Status','Collect')][string]$Action = 'Install',
    # Full path of the game's .exe (the folder our dxgi.dll goes into).
    [string]$GameExe,
    [string]$PackageRoot,
    [switch]$Jitter,
    [switch]$CopyTest
)
# Generic DX11 test install (docs/DX11-HOOKS.md). Adds files next to the game's
# exe, never changes a game file: an existing dxgi.dll (ReShade, another mod) is
# refused, not replaced. Rollback removes exactly what Install added.
# Collect copies this run's logs into the package's logs\ folder.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $PackageRoot) { $PackageRoot = $PSScriptRoot }
$state = Join-Path $PackageRoot 'state'
$manifest = Join-Path $state 'dx11-deployment.json'
$logDir = Join-Path $env:LOCALAPPDATA 'NeuralAlias\dx11'
function Hash($p) { if (Test-Path -LiteralPath $p -PathType Leaf) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { $null } }

if ($Action -eq 'Collect') {
    $out = Join-Path $PackageRoot 'logs'
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    if (Test-Path $logDir) { Get-ChildItem $logDir -Filter '*.log' | Copy-Item -Destination $out -Force }
    $bridgeDir = Join-Path $env:LOCALAPPDATA 'NeuralAlias\bridge'
    if (Test-Path $bridgeDir) { Get-ChildItem $bridgeDir -Filter '*.log' -Recurse | Copy-Item -Destination $out -Force }
    Write-Host "Logs copied to $out"
    return
}
if ($Action -eq 'Status') {
    if (-not (Test-Path $manifest)) { Write-Host 'Not installed by this package.'; return }
    $m = Get-Content -Raw $manifest | ConvertFrom-Json
    Write-Host "Installed into $($m.game_dir) from $($m.source_commit)"
    $m.files | ForEach-Object { [PSCustomObject]@{ File = $_.destination; Unchanged = ((Hash $_.destination) -eq $_.hash) } }
    return
}
if ($Action -eq 'Rollback') {
    if (-not (Test-Path $manifest)) { throw 'Nothing to roll back (no manifest).' }
    $m = Get-Content -Raw $manifest | ConvertFrom-Json
    foreach ($f in $m.files) {
        if (Test-Path -LiteralPath $f.destination) {
            if ((Hash $f.destination) -ne $f.hash) { Write-Warning "Changed since install, kept: $($f.destination)"; continue }
            Remove-Item -LiteralPath $f.destination -Force
        }
    }
    Remove-Item -LiteralPath $manifest -Force
    Write-Host 'Removed the NeuralAlias DX11 files. No game file was changed.'
    return
}

# ---- Install ----
if (-not $GameExe -or -not (Test-Path -LiteralPath $GameExe -PathType Leaf)) { throw 'Give -GameExe with the full path of the game''s .exe.' }
$gameDir = Split-Path -Parent ([IO.Path]::GetFullPath($GameExe))
$exeName = [IO.Path]::GetFileNameWithoutExtension($GameExe)
if (Get-Process -Name $exeName -ErrorAction SilentlyContinue) { throw 'Close the game first.' }
if (Test-Path $manifest) { throw 'Already installed by this package; run Rollback first.' }
$files = @(
    @{ src = 'NeuralAliasDXGI.dll'; dst = 'dxgi.dll' },
    @{ src = 'na_bridge_helper64.exe'; dst = 'na_bridge_helper64.exe' },
    @{ src = 'nvngx_dlss.dll'; dst = 'nvngx_dlss.dll' }
)
foreach ($f in $files) {
    if (-not (Test-Path (Join-Path $PackageRoot $f.src))) { throw "Package is missing $($f.src)." }
    if (Test-Path (Join-Path $gameDir $f.dst)) {
        throw "$($f.dst) already exists in the game folder (another mod such as ReShade, or the game's own DLSS). Not replacing it."
    }
}
$ini = Join-Path $gameDir 'NeuralAliasHost.ini'
if (Test-Path $ini) { throw 'NeuralAliasHost.ini already exists in the game folder. Not replacing it.' }
New-Item -ItemType Directory -Force -Path $state | Out-Null
# Old logs out of the way, so Collect returns only this run.
if (Test-Path $logDir) { Get-ChildItem $logDir -Filter '*.log' | Remove-Item -Force -ErrorAction SilentlyContinue }
$records = @()
foreach ($f in $files) {
    $d = Join-Path $gameDir $f.dst
    Copy-Item -LiteralPath (Join-Path $PackageRoot $f.src) -Destination $d
    $records += [PSCustomObject]@{ destination = $d; hash = (Hash $d) }
}
@"
; NeuralAlias DX11 test (docs/DX11-HOOKS.md). Delete this file (or run Rollback) to turn it off.
[dx11]
enabled=1
jitter=$([int][bool]$Jitter)
mip_bias=-1.0
preset=12
cpu_wait=0
copy_test=$([int][bool]$CopyTest)
"@ | Set-Content -LiteralPath $ini -Encoding ASCII
$records += [PSCustomObject]@{ destination = $ini; hash = (Hash $ini) }
$commit = if (Test-Path (Join-Path $PackageRoot 'commit.txt')) { (Get-Content -Raw (Join-Path $PackageRoot 'commit.txt')).Trim() } else { 'unknown' }
[PSCustomObject]@{ game_dir = $gameDir; source_commit = $commit; files = $records } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifest -Encoding UTF8
Write-Host "Installed into $gameDir (jitter=$([int][bool]$Jitter), copy_test=$([int][bool]$CopyTest)). Play for a minute, quit, then run: .\deploy-dx11.ps1 -Action Collect"
