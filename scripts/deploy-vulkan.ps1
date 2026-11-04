[CmdletBinding()]
param(
    [ValidateSet('Install','Rollback','Status','Collect')][string]$Action = 'Install',
    # Full path of the game's .exe (NeuralAliasHost.ini goes beside it).
    [string]$GameExe,
    [string]$PackageRoot,
    [switch]$Jitter,
    [switch]$CopyTest
)
# Generic Vulkan test install (docs/VULKAN-HOOKS.md). Never changes a game file.
#  - The layer (NeuralAliasVulkan.dll + .json, nvngx_dlss.dll) goes into
#    %LOCALAPPDATA%\NeuralAlias\vulkan\layer, NOT the game folder.
#  - It is registered for this Windows user as an implicit Vulkan layer
#    (HKCU\Software\Khronos\Vulkan\ImplicitLayers). Every Vulkan program then
#    loads it, but it does nothing unless NeuralAliasHost.ini beside that
#    program's exe says [vulkan] enabled=1. Only the game named here gets one.
#  - An existing NeuralAliasHost.ini (for example from the DX11 test) is refused.
# Rollback removes exactly what Install added. Collect copies this run's logs.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $PackageRoot) { $PackageRoot = $PSScriptRoot }
$state = Join-Path $PackageRoot 'state'
$manifest = Join-Path $state 'vulkan-deployment.json'
$base = Join-Path $env:LOCALAPPDATA 'NeuralAlias\vulkan'
$layerDir = Join-Path $base 'layer'
$regKey = 'HKCU:\Software\Khronos\Vulkan\ImplicitLayers'
function Hash($p) { if (Test-Path -LiteralPath $p -PathType Leaf) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { $null } }

if ($Action -eq 'Collect') {
    $out = Join-Path $PackageRoot 'logs'
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    if (Test-Path $base) { Get-ChildItem $base -Filter '*.log' | Copy-Item -Destination $out -Force }
    Write-Host "Logs copied to $out"
    return
}
if ($Action -eq 'Status') {
    if (-not (Test-Path $manifest)) { Write-Host 'Not installed by this package.'; return }
    $m = Get-Content -Raw $manifest | ConvertFrom-Json
    Write-Host "Installed for $($m.game_exe) from $($m.source_commit)"
    $registered = $null -ne (Get-ItemProperty -Path $regKey -Name $m.layer_json -ErrorAction SilentlyContinue)
    Write-Host "Layer registered: $registered"
    $m.files | ForEach-Object { [PSCustomObject]@{ File = $_.destination; Unchanged = ((Hash $_.destination) -eq $_.hash) } }
    return
}
if ($Action -eq 'Rollback') {
    if (-not (Test-Path $manifest)) { throw 'Nothing to roll back (no manifest).' }
    $m = Get-Content -Raw $manifest | ConvertFrom-Json
    if (Test-Path $regKey) { Remove-ItemProperty -Path $regKey -Name $m.layer_json -ErrorAction SilentlyContinue }
    foreach ($f in $m.files) {
        if (Test-Path -LiteralPath $f.destination) {
            if ((Hash $f.destination) -ne $f.hash) { Write-Warning "Changed since install, kept: $($f.destination)"; continue }
            Remove-Item -LiteralPath $f.destination -Force
        }
    }
    if ((Test-Path $layerDir) -and -not (Get-ChildItem $layerDir)) { Remove-Item $layerDir -Force }
    Remove-Item -LiteralPath $manifest -Force
    Write-Host 'Removed the NeuralAlias Vulkan layer and its registration. No game file was changed.'
    return
}

# ---- Install ----
if (-not $GameExe -or -not (Test-Path -LiteralPath $GameExe -PathType Leaf)) { throw 'Give -GameExe with the full path of the game''s .exe.' }
$gameExe = [IO.Path]::GetFullPath($GameExe)
$gameDir = Split-Path -Parent $gameExe
$exeName = [IO.Path]::GetFileNameWithoutExtension($gameExe)
if (Get-Process -Name $exeName -ErrorAction SilentlyContinue) { throw 'Close the game first.' }
if (Test-Path $manifest) { throw 'Already installed by this package; run Rollback first.' }
$files = @('NeuralAliasVulkan.dll', 'NeuralAliasVulkan.json', 'nvngx_dlss.dll')
foreach ($f in $files) { if (-not (Test-Path (Join-Path $PackageRoot $f))) { throw "Package is missing $f." } }
foreach ($f in $files) {
    if (Test-Path (Join-Path $layerDir $f)) { throw "$f is already in $layerDir (another install?). Not replacing it." }
}
$ini = Join-Path $gameDir 'NeuralAliasHost.ini'
if (Test-Path $ini) { throw 'NeuralAliasHost.ini already exists in the game folder (the DX11 test?). Roll that back first. Not replacing it.' }
New-Item -ItemType Directory -Force -Path $state, $layerDir | Out-Null
# Old logs out of the way, so Collect returns only this run.
if (Test-Path $base) { Get-ChildItem $base -Filter '*.log' | Remove-Item -Force -ErrorAction SilentlyContinue }
$records = @()
foreach ($f in $files) {
    $d = Join-Path $layerDir $f
    Copy-Item -LiteralPath (Join-Path $PackageRoot $f) -Destination $d
    $records += [PSCustomObject]@{ destination = $d; hash = (Hash $d) }
}
$json = Join-Path $layerDir 'NeuralAliasVulkan.json'
@"
; NeuralAlias Vulkan test (docs/VULKAN-HOOKS.md). Delete this file (or run Rollback) to turn it off.
[vulkan]
enabled=1
jitter=$([int][bool]$Jitter)
mip_bias=-1.0
preset=12
copy_test=$([int][bool]$CopyTest)
"@ | Set-Content -LiteralPath $ini -Encoding ASCII
$records += [PSCustomObject]@{ destination = $ini; hash = (Hash $ini) }
if (-not (Test-Path $regKey)) { New-Item -Path $regKey -Force | Out-Null }
New-ItemProperty -Path $regKey -Name $json -PropertyType DWord -Value 0 -Force | Out-Null
$commit = if (Test-Path (Join-Path $PackageRoot 'commit.txt')) { (Get-Content -Raw (Join-Path $PackageRoot 'commit.txt')).Trim() } else { 'unknown' }
[PSCustomObject]@{ game_exe = $gameExe; layer_json = $json; source_commit = $commit; files = $records } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifest -Encoding UTF8
Write-Host "Installed for $gameExe (jitter=$([int][bool]$Jitter), copy_test=$([int][bool]$CopyTest)). Play for a minute, quit, then run: .\deploy-vulkan.ps1 -Action Collect"
